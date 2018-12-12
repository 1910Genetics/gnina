#ifndef NNGRIDDER_H
#define NNGRIDDER_H

#include <iostream>
#include <string>
#include <algorithm>
#include <fstream>
#include <boost/program_options.hpp>
#include <boost/multi_array.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/math/quaternion.hpp>
#include <cuda.h>
#include <thrust/system/cuda/experimental/pinned_allocator.h>
#include <vector_types.h>
#include "atom_type.h"
#include "box.h"
#include "gridoptions.h"
#include "molgetter.h"
#include "gridmaker.h"

using namespace std;

/* Maintains atom grid information.  Stores a model of the receptor/ligand with
 * MolGetter, but also numerical grids for every protein/ligand atom type.
 */
class NNGridder {
    //modeled from thurst, but with destroy method to make boost happy
    class float_pinned_allocator {
      public:
        typedef float value_type;
        typedef float* pointer;
        typedef const float* const_pointer;
        typedef float& reference;
        typedef const float& const_reference;
        typedef std::size_t size_type;
        typedef std::ptrdiff_t difference_type;
        bool std_alloc;

        float_pinned_allocator() : std_alloc(true) {
          int count = 0;
          cudaGetDeviceCount(&count);
          if (count) 
            std_alloc = false;
        }

        pointer address(reference r) {
          return &r;
        }
        const_pointer address(const_reference r) {
          return &r;
        }

        //allocate to pinned memory
        pointer allocate(size_type cnt, const_pointer = 0) {
          if (cnt > this->max_size()) {
            throw std::bad_alloc();
          }

          pointer result(0);
          if (std_alloc) {
            std::allocator<value_type> host_allocator;
            result = host_allocator.allocate(cnt);
          }
          else {
            cudaError_t error = cudaMallocHost(reinterpret_cast<void**>(&result),
                cnt * sizeof(value_type));
            if (error) {
              cout << "CUDA error " << cudaGetErrorName(error) << "\n";
              throw std::bad_alloc();
            }
          }

          return result;
        }

        void deallocate(pointer p, size_type cnt) {
          if (std_alloc) {
            std::allocator<value_type> host_allocator;
            host_allocator.deallocate(p, cnt);
          }
          else {
            cudaFreeHost(p);
          }
        }

        size_type max_size() const {
          return (std::numeric_limits<size_type>::max)() / sizeof(float);
        }

        void destroy(float *p) {
        } //don't need to destruct a float

    }; // end pinned_allocator
  public:
    typedef GridMaker::quaternion quaternion;
    typedef boost::multi_array<float, 3, float_pinned_allocator> Grid;

  protected:

    grid_dims dims; //this is a cube
    quaternion Q;
    vec trans;
    double resolution;
    double dimension;
    double radiusmultiple; //extra to consider past vdw radius
    double randtranslate;
    bool binary; //produce binary occupancies
    bool randrotate;
    bool gpu; //use gpu
    bool use_covalent_radius; //instead of xs_radius

    GridMaker* gmaker;
    vector<Grid> receptorGrids;
    vector<Grid> ligandGrids;
    vector<int> rmap; //map atom types to position in grid vectors
    vector<int> lmap;

    vector<float4> recAInfo; //these don't change
    vector<short> recWhichGrid; // the atom type based grid index

    vector<float> ligRadii;
    vector<short> ligWhichGrid; //only change if ligand changes

    vector<Grid> userGrids; //user supplied grids, these set the box

    //gpu data structures, these all point to device mem
    float *gpu_receptorGrids;
    float *gpu_ligandGrids;

    float4 *gpu_receptorAInfo;
    short *gpu_recWhichGrid;

    float4 *gpu_ligandAInfo;
    short *gpu_ligWhichGrid;

    void setRecGPU();
    void setLigGPU();

	  //output a grid the file in map format (for debug)
	  virtual void outputMAPGrid(ostream& out, Grid& grid);

	  //output a grid the file in dx format (for debug)
	  virtual void outputDXGrid(ostream& out, Grid& grid);
    pair<unsigned, unsigned> getrange(const grid_dim& dim, double c, double r);

    //read dx file into grid
    bool readDXGrid(istream& in, vec& center, double& res, Grid& grid);

	  //setup ligand/receptor maps
	  //setup grid dimensions and zero-init
	  virtual void setMapsAndGrids(const gridoptions& opt);
    //return a string representation of the atom type(s) represented by index
    //in map - this isn't particularly efficient, but is only for debug purposes
    string getIndexName(const vector<int>& map, unsigned index) const;

    //set the center of the grid, must reset receptor/ligand
    void setCenter(double x, double y, double z);

    static void cudaCopyGrids(vector<Grid>& grid, float* gpu_grid);

    //for debugging
    static bool compareGrids(Grid& g1, Grid& g2, const char *name, int index);

    float radius(smt sm) { return use_covalent_radius ? covalent_radius(sm) : xs_radius(sm); }

  public:

    NNGridder()
        : resolution(0.5), dimension(24), radiusmultiple(1.5), randtranslate(0),
            binary(false), randrotate(false), gpu(false), use_covalent_radius(false),
            gpu_receptorGrids(NULL), gpu_ligandGrids(NULL),
            gpu_receptorAInfo(NULL), gpu_recWhichGrid(NULL),
            gpu_ligandAInfo(NULL), gpu_ligWhichGrid(NULL) {
    }

    virtual ~NNGridder() {if (gmaker) delete gmaker;}

	  virtual void initialize(const gridoptions& opt);

    //set grids (receptor and ligand)
    //reinits should be set to true if have different molecule than previously scene
    void setModel(const model& m, bool reinitlig = false,
        bool reinitrec = false);

    //return string detailing the configuration (size.channels)
    string getParamString(bool outputrec, bool outputlig) const;

	  //output an AD4 map for each grid
	  virtual void outputMAP(const string& base);

	  //output an dx map for each grid
	  virtual void outputDX(const string& base);

	  //output binary form of raw data in 3D multi-channel form
	  virtual void outputBIN(ostream& out, bool outputrec=true, bool outputlig=true);

	  //set vector to full set of grids
	  virtual void outputMem(vector<float>& out);

    unsigned nchannels() const {
      return receptorGrids.size() + ligandGrids.size() + userGrids.size();
    }

    //for debugging, run non-gpu code and compre to values in current grids
    bool cpuSetModelCheck(const model& m, bool reinitlig = false,
        bool reinitrec = false);
};

/* This gridder uses a MolGetter to read molecules */
class NNMolsGridder : public NNGridder {
  public:
    typedef boost::math::quaternion<double> quaternion;
  private:
    MolGetter mols; //this stores the models

  public:

	  NNMolsGridder(const gridoptions& opt);
    virtual ~NNMolsGridder() {}

    //read a molecule (return false if unsuccessful)
    //set the ligand grid appropriately
    bool readMolecule(bool timeit);

};

class RNNMolsGridder : public NNMolsGridder 
{
public:
  RNNMolsGridder(const gridoptions& opt);
  virtual ~RNNMolsGridder() {}

	//output an AD4 map for each grid
	void outputMAP(const string& base) override;

	//output an dx map for each grid
	void outputDX(const string& base) override;

	//output binary form of raw data in 3D multi-channel form
	void outputBIN(ostream& out, bool outputrec=true, bool outputlig=true) override;

	//set vector to full set of grids
	void outputMem(vector<float>& out) override;
protected:
  double subgrid_dim;
  unsigned ngrids;
  unsigned grid_idx;
	//output a grid the file in map format (for debug)
	void outputMAPGrid(ostream& out, Grid& grid) override;

	//output a grid the file in dx format (for debug)
	void outputDXGrid(ostream& out, Grid& grid) override;
};

#endif
