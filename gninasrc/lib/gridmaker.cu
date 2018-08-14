#include "gridmaker.h"

#include <boost/timer/timer.hpp>
#include <stdio.h>

// GPU routines for gridmaker

#define BLOCKDIM (8)
#define THREADSPERBLOCK (8*8*8)

#define LOG2_WARP_SIZE 5U
#define WARP_SIZE (1U << LOG2_WARP_SIZE)

#ifndef CUDA_CHECK
#define CUDA_CHECK(condition) \
  /* Code block avoids redefinition of cudaError_t error */ \
  do { \
    cudaError_t error = condition; \
    if(error != cudaSuccess) { cerr << " " << cudaGetErrorString(error) << ": " << __FILE__ << ":" << __LINE__ << "\n"; exit(1); } \
  } while (0)
#endif

__shared__ uint scanOutput[THREADSPERBLOCK];
__shared__ uint atomIndices[THREADSPERBLOCK];
__shared__ uint atomMask[THREADSPERBLOCK];
__shared__ uint scanScratch[THREADSPERBLOCK * 2];

//do a scan and return ptr to result (could be either place in double-buffer)
__shared__ uint scanBuffer[2][THREADSPERBLOCK];
__device__ uint* scan(int thid) {
  int pout = 0, pin = 1;
// load input into shared memory.
// This is exclusive scan, so shift right by one and set first elt to 0
  scanBuffer[0][thid] = (thid > 0) ? atomMask[thid - 1] : 0;
  scanBuffer[1][thid] = 0;
  __syncthreads();

  for (int offset = 1; offset < THREADSPERBLOCK; offset *= 2) {
    pout = 1 - pout; // swap double buffer indices
    pin = 1 - pin;
    if (thid >= offset)
      scanBuffer[pout][thid] = scanBuffer[pin][thid]
          + scanBuffer[pin][thid - offset];
    else
      scanBuffer[pout][thid] = scanBuffer[pin][thid];
    __syncthreads();
  }
  return scanBuffer[pout];
}

//Almost the same as naive scan1Inclusive, but doesn't need __syncthreads()
//assuming size <= WARP_SIZE
inline __device__ uint warpScanInclusive(int threadIndex, uint idata,
    volatile uint *s_Data, uint size) {
  uint pos = 2 * threadIndex - (threadIndex & (size - 1));
  s_Data[pos] = 0;
  pos += size;
  s_Data[pos] = idata;

  for (uint offset = 1; offset < size; offset <<= 1)
    s_Data[pos] += s_Data[pos - offset];

  return s_Data[pos];
}

inline __device__ uint warpScanExclusive(int threadIndex, uint idata,
    volatile uint *sScratch, uint size) {
  return warpScanInclusive(threadIndex, idata, sScratch, size) - idata;
}

__inline__ __device__ void sharedMemExclusiveScan(int threadIndex, uint* sInput,
    uint* sOutput) {
  uint idata = sInput[threadIndex];
  //Bottom-level inclusive warp scan
  uint warpResult = warpScanInclusive(threadIndex, idata, scanScratch,
      WARP_SIZE);

  // Save top elements of each warp for exclusive warp scan sync
  // to wait for warp scans to complete (because s_Data is being
  // overwritten)
  __syncthreads();

  if ((threadIndex & (WARP_SIZE - 1)) == (WARP_SIZE - 1)) {
    scanScratch[threadIndex >> LOG2_WARP_SIZE] = warpResult;
  }

  // wait for warp scans to complete
  __syncthreads();

  if (threadIndex < (THREADSPERBLOCK / WARP_SIZE)) {
    // grab top warp elements
    uint val = scanScratch[threadIndex];
    // calculate exclusive scan and write back to shared memory
    scanScratch[threadIndex] = warpScanExclusive(threadIndex, val, scanScratch,
        THREADSPERBLOCK >> LOG2_WARP_SIZE);
  }

  //return updated warp scans with exclusive scan results
  __syncthreads();

  sOutput[threadIndex] = warpResult + scanScratch[threadIndex >> LOG2_WARP_SIZE]
      - idata;
}

//return squared distance between pt and (x,y,z)
__device__
float sqDistance(float4 pt, float x, float y, float z) {
  float ret;
  float tmp = pt.x - x;
  ret = tmp * tmp;
  tmp = pt.y - y;
  ret += tmp * tmp;
  tmp = pt.z - z;
  ret += tmp * tmp;
  return ret;
}

//go through the n atoms referenced in atomIndices and set a grid point
template<bool Binary, typename Dtype> __device__ void set_atoms(float3 origin,
    int dim, float resolution, float rmult, unsigned n, float4 *ainfos, 
    short *gridindex, Dtype *grids) {
  //figure out what grid point we are 
  unsigned xi = threadIdx.x + blockIdx.x * blockDim.x;
  unsigned yi = threadIdx.y + blockIdx.y * blockDim.y;
  unsigned zi = threadIdx.z + blockIdx.z * blockDim.z;

  if(xi >= dim || yi >= dim || zi >= dim)
    return;//bail if we're off-grid, this should not be common

  unsigned gsize = dim * dim * dim;
  //compute x,y,z coordinate of grid point
  float x = xi * resolution + origin.x;
  float y = yi * resolution+origin.y;
  float z = zi * resolution+origin.z;

  //iterate over all atoms
  for(unsigned ai = 0; ai < n; ai++) {
    unsigned i = atomIndices[ai];
    float4 coord = ainfos[i];
    short which = gridindex[i];

    if(which >= 0) { //because of hydrogens on ligands
      float r = ainfos[i].w;
      float rsq = r * r;
      float d = sqDistance(coord, x, y, z);

      if(Binary) {
        if(d < rsq) {
          //set gridpoint to 1
          unsigned goffset = which * gsize;
          unsigned off = (xi * dim + yi) * dim + zi;
          //printf("%f,%f,%f %d,%d,%d  %d  %d %d\n",x,y,z, xi,yi,zi, which, goffset,off);
          grids[goffset + off] = 1.0;
        }
      }
      else {
        //for non binary we want a gaussian were 2 std occurs at the radius
        //after which which switch to a quadratic
        //the quadratic is to fit to have both the same value and first order
        //derivative at the cross over point and a value and derivative of zero
        //at 1.5*radius
        //TODO: figure if we can do the math without sqrt
        float dist = sqrtf(d);
        if (dist < r * rmult) {
          unsigned goffset = which * gsize;
          unsigned off = (xi * dim + yi) * dim + zi;
          unsigned gpos = goffset + off;
          float h = 0.5 * r;

          if (dist <= r) {
            //return gaussian
            float ex = -dist * dist / (2 * h * h);
            grids[gpos] += exp(ex);
          }
          else {//return quadratic
            float eval = 1.0 / (M_E * M_E); //e^(-2)
            float q = dist * dist * eval / (h * h) - 6.0 * eval * dist / h + 9.0 * eval;
            grids[gpos] += q;
          }
        }
      }
    }
  }
}

template<bool Binary, typename Dtype> __device__ void RNNGridMaker::set_atoms(float3 origin,
    unsigned n, float4 *ainfos, short *gridindex, Dtype *grids) {
  //figure out what grid point we are 
  unsigned xi = threadIdx.x + blockIdx.x * blockDim.x;
  unsigned yi = threadIdx.y + blockIdx.y * blockDim.y;
  unsigned zi = threadIdx.z + blockIdx.z * blockDim.z;

  if(xi >= dim || yi >= dim || zi >= dim)
    return;//bail if we're off-grid, this should not be common

  //compute x,y,z coordinate of grid point
  float x = xi * resolution+origin.x;
  float y = yi * resolution+origin.y;
  float z = zi * resolution+origin.z;

  //some facts about our position
  unsigned subgrid_idx_x = xi / subgrid_dim_in_points;
  unsigned subgrid_idx_y = yi / subgrid_dim_in_points;
  unsigned subgrid_idx_z = zi / subgrid_dim_in_points;
  unsigned rel_x = xi % subgrid_dim_in_points;
  unsigned rel_y = yi % subgrid_dim_in_points;
  unsigned rel_z = zi % subgrid_dim_in_points;
  unsigned grid_idx = (((subgrid_idx_x * grids_per_dim) + subgrid_idx_y) * 
      grids_per_dim + subgrid_idx_z);

  //iterate over all atoms
  for(unsigned ai = 0; ai < n; ai++) {
    unsigned i = atomIndices[ai];
    float4 coord = ainfos[i];
    short which = gridindex[i];
    unsigned gpos = ((((grid_idx * batch_size + batch_idx) * ntypes + 
            which) * subgrid_dim_in_points + rel_x) * subgrid_dim_in_points + rel_y) * 
            subgrid_dim_in_points + rel_z;

    if(which >= 0){ //because of hydrogens on ligands
      float r = ainfos[i].w;
      float rsq = r * r;
      float d = sqDistance(coord, x, y, z);

      if(Binary) {
        if(d < rsq) {
          grids[gpos] = 1.0;
        }
      }
      else {
        float dist = sqrtf(d);
        if (dist < r * radiusmultiple) {
          // printf("xi,yi,zi %d %d %d\n", xi, yi, zi);
          float h = 0.5 * r;

          if (dist <= r) {
            //return gaussian
            float ex = -dist * dist / (2 * h * h);
            grids[gpos] += exp(ex);
          }
          else {//return quadratic
            float eval = 1.0 / (M_E * M_E); //e^(-2)
            float q = dist * dist * eval / (h * h) - 6.0 * eval * dist / h + 
              9.0 * eval;
            grids[gpos] += q;
          }
        }
      }
    }
  }
}


//return 1 if atom potentially overlaps block, 0 otherwise
__device__
unsigned atomOverlapsBlock(unsigned aindex, float3 origin, float resolution,
    float4 *ainfos, short *gridindex, float rmult) {

  if (gridindex[aindex] < 0) return 0; //hydrogen

  unsigned xi = blockIdx.x * BLOCKDIM;
  unsigned yi = blockIdx.y * BLOCKDIM;
  unsigned zi = blockIdx.z * BLOCKDIM;

  //compute corners of block
  float startx = xi * resolution + origin.x;
  float starty = yi * resolution + origin.y;
  float startz = zi * resolution + origin.z;

  float endx = startx + resolution * BLOCKDIM;
  float endy = starty + resolution * BLOCKDIM;
  float endz = startz + resolution * BLOCKDIM;

  float r = ainfos[aindex].w * rmult;
  float4 center = ainfos[aindex];

  //does atom overlap box?
  return !((center.x - r > endx) || (center.x + r < startx)
      || (center.y - r > endy) || (center.y + r < starty)
      || (center.z - r > endz) || (center.z + r < startz));
}

__device__
bool scanValid(unsigned idx, uint *scanresult) {
  for (uint i = 1; i < THREADSPERBLOCK; i++) {
    assert(scanresult[i] >= scanresult[i - 1]);
    if (scanresult[i] > scanresult[i - 1]) {
      assert(atomMask[i - 1]);
    }
  }

  return true;
}

//origin is grid origin
//dim is dimension of cubic grid
//resolution is grid resolution
//n is number of atoms
//coords are xyz coors
//gridindex is which grid they belong in
//radii are atom radii
//grids are the output and are assumed to be zeroed
template<bool Binary, typename Dtype> __global__
//__launch_bounds__(THREADSPERBLOCK, 64)
void gpu_grid_set(float3 origin, int dim, float resolution, float rmult, int n,
    float4 *ainfos, short *gridindex, Dtype *grids, bool *mask) {
  unsigned tIndex = ((threadIdx.z * BLOCKDIM) + threadIdx.y) * BLOCKDIM 
    + threadIdx.x;

  //there may be more than THREADPERBLOCK atoms, in which case we have to chunk them
  for(unsigned atomoffset = 0; atomoffset < n; atomoffset += THREADSPERBLOCK) {
    //first parallelize over atoms to figure out if they might overlap this block
    unsigned aindex = atomoffset + tIndex;
    
    if(aindex < n) {
      atomMask[tIndex] = atomOverlapsBlock(aindex, origin, resolution, ainfos, 
          gridindex, rmult);
      if(mask) atomMask[tIndex] &= !(mask[aindex]);
    }
    else {
      atomMask[tIndex] = 0;
    }

    __syncthreads();
    
    //scan the mask to get just relevant indices
    sharedMemExclusiveScan(tIndex, atomMask, scanOutput);
    
    __syncthreads();
    //assert(scanValid(tIndex,scanresult));
    
    //do scatter (stream compaction)
    if(atomMask[tIndex]) {
      atomIndices[scanOutput[tIndex]] = tIndex + atomoffset;
    }
    __syncthreads();

    unsigned nAtoms = scanOutput[THREADSPERBLOCK - 1] 
      + atomMask[THREADSPERBLOCK - 1];
    //atomIndex is now a list of nAtoms atom indices
    set_atoms<Binary, Dtype>(origin, dim, resolution, rmult, nAtoms, ainfos,
        gridindex, grids);
    __syncthreads();//everyone needs to finish before we muck with atomIndices again
  }
}

template<bool Binary, typename Dtype> __global__ 
//__launch_bounds__(THREADSPERBLOCK, 64)
void gpu_grid_set(float3 origin, int n, float4 *ainfos, short *gridindex, Dtype *grids, 
    bool *mask, RNNGridMaker gmaker)
    {
  unsigned tIndex = ((threadIdx.z * BLOCKDIM) + threadIdx.y) * BLOCKDIM 
    + threadIdx.x;

  //there may be more than THREADPERBLOCK atoms, in which case we have to chunk them
  for(unsigned atomoffset = 0; atomoffset < n; atomoffset += THREADSPERBLOCK) {
    //first parallelize over atoms to figure out if they might overlap this block
    unsigned aindex = atomoffset + tIndex;
    
    if(aindex < n) {
      atomMask[tIndex] = atomOverlapsBlock(aindex, origin, 
          gmaker.get_resolution(), ainfos, gridindex,
          gmaker.get_radiusmultiple());
      if(mask) atomMask[tIndex] &= !(mask[aindex]);
    }
    else {
      atomMask[tIndex] = 0;
    }

    __syncthreads();
    
    //scan the mask to get just relevant indices
    sharedMemExclusiveScan(tIndex, atomMask, scanOutput);
    
    __syncthreads();
    //assert(scanValid(tIndex,scanresult));
    
    //do scatter (stream compaction)
    if(atomMask[tIndex])
    {
      atomIndices[scanOutput[tIndex]] = tIndex + atomoffset;
    }
    __syncthreads();

    unsigned nAtoms = scanOutput[THREADSPERBLOCK - 1] 
      + atomMask[THREADSPERBLOCK - 1];
    //atomIndex is now a list of nAtoms atom indices
    gmaker.set_atoms<Binary, Dtype>(origin, nAtoms, ainfos, gridindex, grids);
    __syncthreads();//everyone needs to finish before we muck with atomIndices again
  }
}

//return c rotated by Q, leave w (radius) of coord alone
//this uses the formulat q*v*q^-1, which is simple, but not as computationally
//efficient as precomputing the rotation matrix and using that
__inline__  __device__ float4 applyQ(float4 coord, float3 center, qt Q) {
  //apply rotation
  float3 p = Q.rotate(coord.x - center.x, coord.y - center.y,
      coord.z - center.z);
  p += center;
  return make_float4(p.x, p.y, p.z, coord.w);
}

//apply quaternion Q to coordinates in ainfos
__global__
void gpu_coord_rotate(float3 center, qt Q, int n, float4 *ainfos) {
  unsigned aindex = blockIdx.x * blockDim.x + threadIdx.x;
  if (aindex < n) {
    float4 ai = ainfos[aindex];
    float4 rotated = applyQ(ai, center, Q);
    ainfos[aindex] = rotated;
  }
}

//set mask bits to one if atom should be ignored
__global__
void gpu_mask_atoms(float3 gridcenter, float rsq, int n, float4 *ainfos,
    bool *mask) {
  unsigned aindex = blockIdx.x * blockDim.x + threadIdx.x;
  if (aindex < n) {
    float4 ai = ainfos[aindex];
    float xdiff = ai.x - gridcenter.x;
    float ydiff = ai.y - gridcenter.y;
    float zdiff = ai.z - gridcenter.z;
    float distsq = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;
    if (distsq > rsq) {
      mask[aindex] = 1;
    } else {
      mask[aindex] = 0;
    }
  }
}

//WARNING: if Q is not the identify, will modify coordinates in ainfos in-place
template<typename Dtype>
void GridMaker::setAtomsGPU(unsigned natoms,float4 *ainfos,short *gridindex,
    qt Q, unsigned ngrids,Dtype *grids) {
  //each thread is responsible for a grid point location and will handle all atom types
  //each block is 8x8x8=512 threads
  float3 origin(dims[0].x, dims[1].x, dims[2].x); //actually a gfloat3
  dim3 threads(BLOCKDIM, BLOCKDIM, BLOCKDIM);
  unsigned blocksperside = ceil(dim / float(BLOCKDIM));
  dim3 blocks(blocksperside, blocksperside, blocksperside);
  unsigned gsize = ngrids * dim * dim * dim;
  CUDA_CHECK(cudaMemset(grids, 0, gsize * sizeof(float)));  //TODO: see if faster to do in kernel - it isn't, but this still may not be fastest
  
  if(natoms == 0) return;

  bool *mask = NULL;
  unsigned natomblocks = (natoms + THREADSPERBLOCK - 1) / THREADSPERBLOCK;
  if(spherize) {
    cudaMalloc(&mask, sizeof(bool) * natoms);
    cudaMemset(mask, 0, sizeof(bool) * natoms);
    gpu_mask_atoms<<<natomblocks, THREADSPERBLOCK>>>(center, rsq, natoms, ainfos, mask);
    CUDA_CHECK(cudaPeekAtLastError());
  }

  if(Q.R_component_1() != 0) { 
    qt q(Q);
    gpu_coord_rotate<<<natomblocks, THREADSPERBLOCK>>>(center, q, natoms, 
        ainfos);
  }
  if(binary) {
    gpu_grid_set<true> <<<blocks, threads>>>(origin, dim, resolution,
        radiusmultiple, natoms, ainfos, gridindex, grids, mask);
    CUDA_CHECK (cudaPeekAtLastError());
  }
  else {
    gpu_grid_set<false> <<<blocks, threads>>>(origin, dim, resolution,
        radiusmultiple, natoms, ainfos, gridindex, grids, mask);
    CUDA_CHECK(cudaPeekAtLastError());
  }
  CUDA_CHECK(cudaStreamSynchronize(cudaStreamPerThread)); //this was removed, pretty sure we want it

  if(mask) {
    cudaFree(mask);
  }
}

template<typename Dtype>
void RNNGridMaker::setAtomsGPU(unsigned natoms,float4 *ainfos,short *gridindex,
    qt Q, unsigned ngrids, Dtype *grids) {
  //each thread is responsible for a grid point location and will handle all atom types
  //each block is 8x8x8=512 threads
  float3 origin(dims[0].x, dims[1].x, dims[2].x); //actually a gfloat3
  unsigned ncubes = grids_per_dim * grids_per_dim * grids_per_dim;
  dim3 threads(BLOCKDIM, BLOCKDIM, BLOCKDIM);
  unsigned blocksperside = ceil(dim / float(BLOCKDIM));
  dim3 blocks(blocksperside, blocksperside, blocksperside);
  unsigned gsize = ntypes * batch_size * ncubes * subgrid_dim_in_points * subgrid_dim_in_points *
        subgrid_dim_in_points;
  if (batch_idx == 0)
    CUDA_CHECK(cudaMemset(grids, 0, gsize * sizeof(float)));  //TODO: see if faster to do in kernel - it isn't, but this still may not be fastest
  
  if(natoms == 0) return;

  bool *mask = NULL;
  unsigned natomblocks = (natoms + THREADSPERBLOCK - 1) / THREADSPERBLOCK;
  if(spherize) {
    cudaMalloc(&mask, sizeof(bool) * natoms);
    cudaMemset(mask, 0, sizeof(bool) * natoms);
    gpu_mask_atoms<<<natomblocks, THREADSPERBLOCK>>>(center, rsq, natoms, 
        ainfos, mask);
    CUDA_CHECK(cudaPeekAtLastError());
  }

  if(Q.R_component_1() != 0) { 
    qt q(Q);
    gpu_coord_rotate<<<natomblocks, THREADSPERBLOCK>>>(center, q, natoms, 
        ainfos);
  }
  if(binary) {
    gpu_grid_set<true> <<<blocks, threads>>>(origin, natoms, ainfos, gridindex, 
        grids, mask, *this);
    CUDA_CHECK (cudaPeekAtLastError());
  }
  else {
    gpu_grid_set<false> <<<blocks, threads>>>(origin, natoms, ainfos, gridindex, 
        grids, mask, *this);
    CUDA_CHECK(cudaPeekAtLastError());
  }
  CUDA_CHECK(cudaStreamSynchronize(cudaStreamPerThread)); 

  if(mask) {
    cudaFree(mask);
  }
  batch_idx = (batch_idx + 1) % batch_size;
}

//instantiations
template
void GridMaker::setAtomsGPU(unsigned natoms, float4 *ainfos, short *gridindex,
    qt Q, unsigned ngrids, float *grids);
template
void GridMaker::setAtomsGPU(unsigned natoms, float4 *ainfos, short *gridindex,
    qt Q, unsigned ngrids, double *grids);

template
void RNNGridMaker::setAtomsGPU(unsigned natoms, float4 *ainfos, short *gridindex,
    qt Q, unsigned ngrids, float *grids);
template
void RNNGridMaker::setAtomsGPU(unsigned natoms, float4 *ainfos, short *gridindex,
    qt Q, unsigned ngrids, double *grids);
