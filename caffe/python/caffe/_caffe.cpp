#include <Python.h>  // NOLINT(build/include_alpha)

// Produce deprecation warnings (needs to come before arrayobject.h inclusion).
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <boost/make_shared.hpp>
#include <boost/python.hpp>
#include <boost/python/raw_function.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>
#include <numpy/arrayobject.h>

// these need to be included after boost on OS X
#include <string>  // NOLINT(build/include_order)
#include <vector>  // NOLINT(build/include_order)
#include <fstream>  // NOLINT

#include "caffe/caffe.hpp"
#include "caffe/layers/memory_data_layer.hpp"
#include "caffe/layers/python_layer.hpp"
#include "caffe/layers/pooling_layer.hpp"
#include "caffe/layers/molgrid_data_layer.hpp"
#include "caffe/sgd_solvers.hpp"

// Temporary solution for numpy < 1.7 versions: old macro, no promises.
// You're strongly advised to upgrade to >= 1.7.
#ifndef NPY_ARRAY_C_CONTIGUOUS
#define NPY_ARRAY_C_CONTIGUOUS NPY_C_CONTIGUOUS
#define PyArray_SetBaseObject(arr, x) (PyArray_BASE(arr) = (x))
#endif

/* Fix to avoid registration warnings in pycaffe (#3960) */
#define BP_REGISTER_SHARED_PTR_TO_PYTHON(PTR) do { \
  const boost::python::type_info info = \
    boost::python::type_id<shared_ptr<PTR > >(); \
  const boost::python::converter::registration* reg = \
    boost::python::converter::registry::query(info); \
  if (reg == NULL) { \
    bp::register_ptr_to_python<shared_ptr<PTR > >(); \
  } else if ((*reg).m_to_python == NULL) { \
    bp::register_ptr_to_python<shared_ptr<PTR > >(); \
  } \
} while (0)

namespace bp = boost::python;

namespace caffe {

//forward declarations
enum PoolingParameter_PoolMethod;

// For Python, for now, we'll just always use float as the type.
typedef float Dtype;
const int NPY_DTYPE = NPY_FLOAT32;

// Selecting mode.
void set_mode_cpu() { Caffe::set_mode(Caffe::CPU); }
void set_mode_gpu() { Caffe::set_mode(Caffe::GPU); }

// function for allowing toggling input max pool to ave
// pool for backprop during dreamlike input optimization
bool toggle_max_to_ave(Net<Dtype>& net) {
  const vector<caffe::shared_ptr<Layer<Dtype> > >& layers = net.layers();
  PoolingLayer<Dtype> *pool = NULL;
  for (unsigned i = 1, nl = layers.size(); i < nl; i++) {
    pool = dynamic_cast<PoolingLayer<Dtype>*>(layers[i].get());
    if (pool)
      break; //found it
    else
      if (layers[i]->type() == string("Convolution"))
        break; 
      else if (layers[i]->type() == string("InnerProduct")) 
        break;
  }

  if (pool) {
    if (pool->pool() == PoolingParameter_PoolMethod_MAX) {
      pool->set_pool(PoolingParameter_PoolMethod_AVE);
    } else {
      pool = NULL; //no need to reset to max
    }
  }
  return pool;
}

void toggle_ave_to_max(Net<Dtype>& net) {
  const vector<caffe::shared_ptr<Layer<Dtype> > >& layers = net.layers();
  PoolingLayer<Dtype> *pool = NULL;
  for (unsigned i = 1, nl = layers.size(); i < nl; i++) {
    pool = dynamic_cast<PoolingLayer<Dtype>*>(layers[i].get());
    if (pool)
      break; //found it
    else
      if (layers[i]->type() == string("Convolution"))
        break; 
      else if (layers[i]->type() == string("InnerProduct")) 
        break;
  }

  if (pool) {
    if (pool->pool() == PoolingParameter_PoolMethod_AVE) {
      pool->set_pool(PoolingParameter_PoolMethod_MAX);
    } 
  }
}


void InitLog() {
  ::google::InitGoogleLogging("");
  ::google::InstallFailureSignalHandler();
}
void InitLogLevel(int level) {
  FLAGS_minloglevel = level;
  InitLog();
}
void InitLogLevelPipe(int level, bool stderr) {
  FLAGS_minloglevel = level;
  FLAGS_logtostderr = stderr;
  InitLog();
}
void Log(const string& s) {
  LOG(INFO) << s;
}

void set_random_seed(unsigned int seed) { Caffe::set_random_seed(seed); }

// For convenience, check that input files can be opened, and raise an
// exception that boost will send to Python if not (caffe could still crash
// later if the input files are disturbed before they are actually used, but
// this saves frustration in most cases).
static void CheckFile(const string& filename) {
    std::ifstream f(filename.c_str());
    if (!f.good()) {
      f.close();
      throw std::runtime_error("Could not open file " + filename);
    }
    f.close();
}

void CheckContiguousArray(PyArrayObject* arr, string name,
    int channels, int height, int width) {
  if (!(PyArray_FLAGS(arr) & NPY_ARRAY_C_CONTIGUOUS)) {
    throw std::runtime_error(name + " must be C contiguous");
  }
  if (PyArray_NDIM(arr) != 4) {
    throw std::runtime_error(name + " must be 4-d");
  }
  if (PyArray_TYPE(arr) != NPY_FLOAT32) {
    throw std::runtime_error(name + " must be float32");
  }
  if (PyArray_DIMS(arr)[1] != channels) {
    throw std::runtime_error(name + " has wrong number of channels");
  }
  if (PyArray_DIMS(arr)[2] != height) {
    throw std::runtime_error(name + " has wrong height");
  }
  if (PyArray_DIMS(arr)[3] != width) {
    throw std::runtime_error(name + " has wrong width");
  }
}

// Net constructor
shared_ptr<Net<Dtype> > Net_Init(string network_file, int phase,
    const int level, const bp::object& stages,
    const bp::object& weights) {
  CheckFile(network_file);

  // Convert stages from list to vector
  vector<string> stages_vector;
  if (!stages.is_none()) {
    for (int i = 0; i < len(stages); i++) {
      stages_vector.push_back(bp::extract<string>(stages[i]));
    }
  }

  // Initialize net
  shared_ptr<Net<Dtype> > net(new Net<Dtype>(network_file,
        static_cast<Phase>(phase), level, &stages_vector));

  // Load weights
  if (!weights.is_none()) {
    std::string weights_file_str = bp::extract<std::string>(weights);
    CheckFile(weights_file_str);
    net->CopyTrainedLayersFrom(weights_file_str);
  }

  return net;
}

// Legacy Net construct-and-load convenience constructor
shared_ptr<Net<Dtype> > Net_Init_Load(
    string param_file, string pretrained_param_file, int phase) {
  LOG(WARNING) << "DEPRECATION WARNING - deprecated use of Python interface";
  LOG(WARNING) << "Use this instead (with the named \"weights\""
    << " parameter):";
  LOG(WARNING) << "Net('" << param_file << "', " << phase
    << ", weights='" << pretrained_param_file << "')";
  CheckFile(param_file);
  CheckFile(pretrained_param_file);

  shared_ptr<Net<Dtype> > net(new Net<Dtype>(param_file,
      static_cast<Phase>(phase)));
  net->CopyTrainedLayersFrom(pretrained_param_file);
  return net;
}

void Net_Save(const Net<Dtype>& net, string filename) {
  NetParameter net_param;
  net.ToProto(&net_param, false);
  WriteProtoToBinaryFile(net_param, filename.c_str());
}

void Net_SaveHDF5(const Net<Dtype>& net, string filename) {
  net.ToHDF5(filename);
}

void Net_LoadHDF5(Net<Dtype>* net, string filename) {
  net->CopyTrainedLayersFromHDF5(filename.c_str());
}

void Net_SetInputArrays(Net<Dtype>* net, bp::object data_obj,
    bp::object labels_obj) {
  // check that this network has an input MemoryDataLayer
  shared_ptr<MemoryDataLayer<Dtype> > md_layer =
    boost::dynamic_pointer_cast<MemoryDataLayer<Dtype> >(net->layers()[0]);
  if (!md_layer) {
    throw std::runtime_error("set_input_arrays may only be called if the"
        " first layer is a MemoryDataLayer");
  }

  // check that we were passed appropriately-sized contiguous memory
  PyArrayObject* data_arr =
      reinterpret_cast<PyArrayObject*>(data_obj.ptr());
  PyArrayObject* labels_arr =
      reinterpret_cast<PyArrayObject*>(labels_obj.ptr());
  CheckContiguousArray(data_arr, "data array", md_layer->shape()[1],
      md_layer->shape()[2], md_layer->shape()[3]);
  CheckContiguousArray(labels_arr, "labels array", 1, 1, 1);
  if (PyArray_DIMS(data_arr)[0] != PyArray_DIMS(labels_arr)[0]) {
    throw std::runtime_error("data and labels must have the same first"
        " dimension");
  }
  if (PyArray_DIMS(data_arr)[0] % md_layer->batch_size() != 0) {
    throw std::runtime_error("first dimensions of input arrays must be a"
        " multiple of batch size");
  }

  md_layer->Reset(static_cast<Dtype*>(PyArray_DATA(data_arr)),
      static_cast<Dtype*>(PyArray_DATA(labels_arr)),
      PyArray_DIMS(data_arr)[0]);
}

Solver<Dtype>* GetSolverFromFile(const string& filename) {
  SolverParameter param;
  ReadSolverParamsFromTextFileOrDie(filename, &param);
  return SolverRegistry<Dtype>::CreateSolver(param);
}

struct NdarrayConverterGenerator {
  template <typename T> struct apply;
};

template <>
struct NdarrayConverterGenerator::apply<Dtype*> {
  struct type {
    PyObject* operator() (Dtype* data) const {
      // Just store the data pointer, and add the shape information in postcall.
      return PyArray_SimpleNewFromData(0, NULL, NPY_DTYPE, data);
    }
    const PyTypeObject* get_pytype() {
      return &PyArray_Type;
    }
  };
};

struct NdarrayCallPolicies : public bp::default_call_policies {
  typedef NdarrayConverterGenerator result_converter;
  PyObject* postcall(PyObject* pyargs, PyObject* result) {
    bp::object pyblob = bp::extract<bp::tuple>(pyargs)()[0];
    shared_ptr<Blob<Dtype> > blob =
      bp::extract<shared_ptr<Blob<Dtype> > >(pyblob);
    // Free the temporary pointer-holding array, and construct a new one with
    // the shape information from the blob.
    void* data = PyArray_DATA(reinterpret_cast<PyArrayObject*>(result));
    Py_DECREF(result);
    const int num_axes = blob->num_axes();
    vector<npy_intp> dims(blob->shape().begin(), blob->shape().end());
    PyObject *arr_obj = PyArray_SimpleNewFromData(num_axes, dims.data(),
                                                  NPY_FLOAT32, data);
    // SetBaseObject steals a ref, so we need to INCREF.
    Py_INCREF(pyblob.ptr());
    PyArray_SetBaseObject(reinterpret_cast<PyArrayObject*>(arr_obj),
        pyblob.ptr());
    return arr_obj;
  }
};

bp::object Blob_Reshape(bp::tuple args, bp::dict kwargs) {
  if (bp::len(kwargs) > 0) {
    throw std::runtime_error("Blob.reshape takes no kwargs");
  }
  Blob<Dtype>* self = bp::extract<Blob<Dtype>*>(args[0]);
  vector<int> shape(bp::len(args) - 1);
  for (int i = 1; i < bp::len(args); ++i) {
    shape[i - 1] = bp::extract<int>(args[i]);
  }
  self->Reshape(shape);
  // We need to explicitly return None to use bp::raw_function.
  return bp::object();
}

bp::object BlobVec_add_blob(bp::tuple args, bp::dict kwargs) {
  if (bp::len(kwargs) > 0) {
    throw std::runtime_error("BlobVec.add_blob takes no kwargs");
  }
  typedef vector<shared_ptr<Blob<Dtype> > > BlobVec;
  BlobVec* self = bp::extract<BlobVec*>(args[0]);
  vector<int> shape(bp::len(args) - 1);
  for (int i = 1; i < bp::len(args); ++i) {
    shape[i - 1] = bp::extract<int>(args[i]);
  }
  self->push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
  // We need to explicitly return None to use bp::raw_function.
  return bp::object();
}

template<typename Dtype>
class SolverCallback: public Solver<Dtype>::Callback {
 protected:
  bp::object on_start_, on_gradients_ready_;

 public:
  SolverCallback(bp::object on_start, bp::object on_gradients_ready)
    : on_start_(on_start), on_gradients_ready_(on_gradients_ready) { }
  virtual void on_gradients_ready() {
    on_gradients_ready_();
  }
  virtual void on_start() {
    on_start_();
  }
};
template<typename Dtype>
void Solver_add_callback(Solver<Dtype> * solver, bp::object on_start,
  bp::object on_gradients_ready) {
  solver->add_callback(new SolverCallback<Dtype>(on_start, on_gradients_ready));
}

// Seems boost cannot call the base method directly
void Solver_add_nccl(Solver<Dtype>* solver
#ifdef USE_NCCL
  , NCCL<Dtype>* nccl
#endif
) {
#ifdef USE_NCCL
  solver->add_callback(nccl);
#endif
}

void share_weights(Solver<Dtype>* solver, Net<Dtype>* net) {
  net->ShareTrainedLayersWith(solver->net().get());
}

template<typename Dtype>
class NetCallback: public Net<Dtype>::Callback {
 public:
  explicit NetCallback(bp::object run) : run_(run) {}

 protected:
  virtual void run(int layer) {
    run_(layer);
  }
  bp::object run_;
};
void Net_before_forward(Net<Dtype>* net, bp::object run) {
  net->add_before_forward(new NetCallback<Dtype>(run));
}
void Net_after_forward(Net<Dtype>* net, bp::object run) {
  net->add_after_forward(new NetCallback<Dtype>(run));
}
void Net_before_backward(Net<Dtype>* net, bp::object run) {
  net->add_before_backward(new NetCallback<Dtype>(run));
}
void Net_after_backward(Net<Dtype>* net, bp::object run) {
  net->add_after_backward(new NetCallback<Dtype>(run));
}

void Net_add_nccl(Net<Dtype>* net
#ifdef USE_NCCL
  , NCCL<Dtype>* nccl
#endif
) {
#ifdef USE_NCCL
  net->add_after_backward(nccl);
#endif
}
#ifndef USE_NCCL
template<typename Dtype>
class NCCL {
 public:
  NCCL(shared_ptr<Solver<Dtype> > solver, const string& uid) {}
};
#endif

bool HasNCCL() {
#ifdef USE_NCCL
  return true;
#else
  return false;
#endif
}

#ifdef USE_NCCL
bp::object NCCL_New_Uid() {
  std::string uid = NCCL<Dtype>::new_uid();
#if PY_MAJOR_VERSION >= 3
  // Convert std::string to bytes so that Python does not
  // try to decode the string using the current locale.

  // Since boost 1.53 boost.python will convert str and bytes
  // to std::string but will convert std::string to str. Here we
  // force a bytes object to be returned. When this object
  // is passed back to the NCCL constructor boost.python will
  // correctly convert the bytes to std::string automatically
  PyObject* py_uid = PyBytes_FromString(uid.c_str());
  return bp::object(bp::handle<>(py_uid));
#else
  // automatic conversion is correct for python 2.
  return bp::object(uid);
#endif
}
#endif

//register a vector of the specified type using name, but only if it isn't already registered
template <typename T>
void register_vector_type(const char *name) {
  bp::type_info info = bp::type_id< std::vector<T> >();
  const bp::converter::registration* reg = bp::converter::registry::query(info);
  if (reg == NULL || (*reg).m_to_python == NULL) {
    //register the type
    bp::class_<std::vector<T> >(name)
        .def(bp::vector_indexing_suite<std::vector<T> >());
  }
}


BOOST_PYTHON_MEMBER_FUNCTION_OVERLOADS(SolveOverloads, Solve, 0, 1);

BOOST_PYTHON_MODULE(_caffe) {
  // below, we prepend an underscore to methods that will be replaced
  // in Python

  bp::scope().attr("__version__") = AS_STRING(CAFFE_VERSION);

  // Caffe utility functions
  bp::def("init_log", &InitLog);
  bp::def("init_log", &InitLogLevel);
  bp::def("init_log", &InitLogLevelPipe);
  bp::def("log", &Log);
  bp::def("has_nccl", &HasNCCL);
  bp::def("set_mode_cpu", &set_mode_cpu);
  bp::def("set_mode_gpu", &set_mode_gpu);
  bp::def("set_random_seed", &set_random_seed);
  bp::def("set_device", &Caffe::SetDevice);
  bp::def("solver_count", &Caffe::solver_count);
  bp::def("set_solver_count", &Caffe::set_solver_count);
  bp::def("solver_rank", &Caffe::solver_rank);
  bp::def("set_solver_rank", &Caffe::set_solver_rank);
  bp::def("set_multiprocess", &Caffe::set_multiprocess);
  bp::def("device_synchronize", &Caffe::device_synchronize);

  bp::def("layer_type_list", &LayerRegistry<Dtype>::LayerTypeList);
  bp::def("toggle_max_to_ave", &toggle_max_to_ave);
  bp::def("toggle_ave_to_max", &toggle_ave_to_max);

  bp::class_<Net<Dtype>, shared_ptr<Net<Dtype> >, boost::noncopyable >("Net",
    bp::no_init)
    // Constructor
    .def("__init__", bp::make_constructor(&Net_Init,
          bp::default_call_policies(), (bp::arg("network_file"), "phase",
            bp::arg("level")=0, bp::arg("stages")=bp::object(),
            bp::arg("weights")=bp::object())))
    // Legacy constructor
    .def("__init__", bp::make_constructor(&Net_Init_Load))
    .def("_forward", &Net<Dtype>::ForwardFromTo)
    .def("_backward", &Net<Dtype>::BackwardFromTo)
    .def("clearblobs", &Net<Dtype>::ClearBlobs)
    .def("reshape", &Net<Dtype>::Reshape)
    .def("clear_param_diffs", &Net<Dtype>::ClearParamDiffs)
    // The cast is to select a particular overload.
    .def("copy_from", static_cast<void (Net<Dtype>::*)(const string)>(
        &Net<Dtype>::CopyTrainedLayersFrom))
    .def("share_with", &Net<Dtype>::ShareTrainedLayersWith)
    .add_property("_blob_loss_weights", bp::make_function(
        &Net<Dtype>::blob_loss_weights, bp::return_internal_reference<>()))
    .def("_bottom_ids", bp::make_function(&Net<Dtype>::bottom_ids,
        bp::return_value_policy<bp::copy_const_reference>()))
    .def("_top_ids", bp::make_function(&Net<Dtype>::top_ids,
        bp::return_value_policy<bp::copy_const_reference>()))
    .add_property("_blobs", bp::make_function(&Net<Dtype>::blobs,
        bp::return_internal_reference<>()))
    .add_property("layers", bp::make_function(&Net<Dtype>::layers,
        bp::return_internal_reference<>()))
    .add_property("_blob_names", bp::make_function(&Net<Dtype>::blob_names,
        bp::return_value_policy<bp::copy_const_reference>()))
    .add_property("_layer_names", bp::make_function(&Net<Dtype>::layer_names,
        bp::return_value_policy<bp::copy_const_reference>()))
    .add_property("_inputs", bp::make_function(&Net<Dtype>::input_blob_indices,
        bp::return_value_policy<bp::copy_const_reference>()))
    .add_property("_outputs",
        bp::make_function(&Net<Dtype>::output_blob_indices,
        bp::return_value_policy<bp::copy_const_reference>()))
    .def("_set_input_arrays", &Net_SetInputArrays,
        bp::with_custodian_and_ward<1, 2, bp::with_custodian_and_ward<1, 3> >())
    .def("save", &Net_Save)
    .def("save_hdf5", &Net_SaveHDF5)
    .def("load_hdf5", &Net_LoadHDF5)
    .def("before_forward", &Net_before_forward)
    .def("after_forward", &Net_after_forward)
    .def("before_backward", &Net_before_backward)
    .def("after_backward", &Net_after_backward)
    .def("after_backward", &Net_add_nccl);
  BP_REGISTER_SHARED_PTR_TO_PYTHON(Net<Dtype>);

  bp::class_<Blob<Dtype>, shared_ptr<Blob<Dtype> >, boost::noncopyable>(
    "Blob", bp::no_init)
    .add_property("shape",
        bp::make_function(
            static_cast<const vector<int>& (Blob<Dtype>::*)() const>(
                &Blob<Dtype>::shape),
            bp::return_value_policy<bp::copy_const_reference>()))
    .add_property("num",      &Blob<Dtype>::num)
    .add_property("channels", &Blob<Dtype>::channels)
    .add_property("height",   &Blob<Dtype>::height)
    .add_property("width",    &Blob<Dtype>::width)
    .add_property("count",    static_cast<int (Blob<Dtype>::*)() const>(
        &Blob<Dtype>::count))
    .def("reshape",           bp::raw_function(&Blob_Reshape))
    .def("clear", bp::make_function(&Blob<Dtype>::Clear))
    .def("copyfrom", &Blob<Dtype>::CopyFrom, (bp::arg("blob"), bp::arg("copy_diff") = false, bp::arg("reshape") = false))
    .def("set_data", &Blob<Dtype>::set_data)
    .def("set_diff", &Blob<Dtype>::set_diff)
    .def("scale_data", &Blob<Dtype>::scale_data)
    .def("scale_diff", &Blob<Dtype>::scale_diff)
#ifndef CPU_ONLY
    .add_property("_gpu_data_ptr",
        reinterpret_cast<uintptr_t (Blob<Dtype>::*)()>(
          &Blob<Dtype>::mutable_gpu_data))
    .add_property("_gpu_diff_ptr",
        reinterpret_cast<uintptr_t (Blob<Dtype>::*)()>(
          &Blob<Dtype>::mutable_gpu_diff))
#endif
    .add_property("data",     bp::make_function(&Blob<Dtype>::mutable_cpu_data,
          NdarrayCallPolicies()))
    .add_property("diff",     bp::make_function(&Blob<Dtype>::mutable_cpu_diff,
          NdarrayCallPolicies()));
  BP_REGISTER_SHARED_PTR_TO_PYTHON(Blob<Dtype>);

  bp::class_<Layer<Dtype>, shared_ptr<PythonLayer<Dtype> >,
    boost::noncopyable>("Layer", bp::init<const LayerParameter&>())
    .add_property("blobs", bp::make_function(&Layer<Dtype>::blobs,
          bp::return_internal_reference<>()))
    .def("setup", &Layer<Dtype>::LayerSetUp)
    .def("reshape", &Layer<Dtype>::Reshape)
    .def("clear", &Layer<Dtype>::Clear)
    .add_property("type", bp::make_function(&Layer<Dtype>::type));
  BP_REGISTER_SHARED_PTR_TO_PYTHON(Layer<Dtype>);

  bp::class_<MolGridDataLayer<Dtype>, bp::bases<Layer<Dtype> >,
    shared_ptr<MolGridDataLayer<Dtype> >, boost::noncopyable>("MolGridDataLayer", bp::no_init)
    .def("get_mol_info", +[](const MolGridDataLayer<Dtype>& self, int i) { return self.getMolInfo(i);})
    .def("get_moltransform", +[](const MolGridDataLayer<Dtype>& self, int i) { return self.getMolInfo(i).transform;})
    .def("get_grid_center", +[](MolGridDataLayer<Dtype>& self) { vec c = self.getGridCenter(); return std::vector<float>{c[0],c[1],c[2]};})
    .def("get_rec_types", &MolGridDataLayer<Dtype>::getRecTypes)
    .def("get_lig_types", &MolGridDataLayer<Dtype>::getLigTypes);

  BP_REGISTER_SHARED_PTR_TO_PYTHON(MolGridDataLayer<Dtype>);

  bp::class_<MolGridDataLayer<Dtype>::mol_info>("mol_info")
      .def_readonly("transform",&MolGridDataLayer<Dtype>::mol_info::transform);

  bp::class_<vec>("vec")
      .def("x",&vec::x)
      .def("y",&vec::y)
      .def("z",&vec::z);

  bp::class_<qt>("qt")
      .def_readwrite("a",&qt::a)
      .def_readwrite("b",&qt::b)
      .def_readwrite("c",&qt::c)
      .def_readwrite("d",&qt::d);


  bp::class_<SolverParameter>("SolverParameter", bp::no_init)
    .add_property("max_iter", &SolverParameter::max_iter)
    .add_property("display", &SolverParameter::display)
    .add_property("layer_wise_reduce", &SolverParameter::layer_wise_reduce);
  bp::class_<LayerParameter>("LayerParameter", bp::no_init);

  bp::class_<Solver<Dtype>, shared_ptr<Solver<Dtype> >, boost::noncopyable>(
    "Solver", bp::no_init)
    .add_property("net", &Solver<Dtype>::net)
    .add_property("test_nets", bp::make_function(&Solver<Dtype>::test_nets,
          bp::return_internal_reference<>()))
    .add_property("iter", &Solver<Dtype>::iter)
    .def("add_callback", &Solver_add_callback<Dtype>)
    .def("add_callback", &Solver_add_nccl)
    .def("solve", static_cast<void (Solver<Dtype>::*)(const char*)>(
          &Solver<Dtype>::Solve), SolveOverloads())
    .def("step", &Solver<Dtype>::Step)
    .def("apply_update", &Solver<Dtype>::ApplyUpdate)
    .def("increment_iter", &Solver<Dtype>::increment_iter)
    .def("restore", &Solver<Dtype>::Restore)
    .def("testall", &Solver<Dtype>::TestAll)
    .def("get_base_lr", &Solver<Dtype>::get_base_lr)
    .def("set_base_lr", &Solver<Dtype>::set_base_lr)
    .def("snapshot", &Solver<Dtype>::Snapshot)
    .def("share_weights", &share_weights)
    .add_property("param", bp::make_function(&Solver<Dtype>::param,
              bp::return_value_policy<bp::copy_const_reference>()));
  BP_REGISTER_SHARED_PTR_TO_PYTHON(Solver<Dtype>);

  bp::class_<SGDSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<SGDSolver<Dtype> >, boost::noncopyable>(
        "SGDSolver", bp::init<string>());
  bp::class_<NesterovSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<NesterovSolver<Dtype> >, boost::noncopyable>(
        "NesterovSolver", bp::init<string>());
  bp::class_<AdaGradSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<AdaGradSolver<Dtype> >, boost::noncopyable>(
        "AdaGradSolver", bp::init<string>());
  bp::class_<RMSPropSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<RMSPropSolver<Dtype> >, boost::noncopyable>(
        "RMSPropSolver", bp::init<string>());
  bp::class_<AdaDeltaSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<AdaDeltaSolver<Dtype> >, boost::noncopyable>(
        "AdaDeltaSolver", bp::init<string>());
  bp::class_<AdamSolver<Dtype>, bp::bases<Solver<Dtype> >,
    shared_ptr<AdamSolver<Dtype> >, boost::noncopyable>(
        "AdamSolver", bp::init<string>());

  bp::def("get_solver", &GetSolverFromFile,
      bp::return_value_policy<bp::manage_new_object>());

  // vector wrappers for all the vector types we use
  bp::class_<vector<shared_ptr<Blob<Dtype> > > >("BlobVec")
    .def(bp::vector_indexing_suite<vector<shared_ptr<Blob<Dtype> > >, true>())
    .def("add_blob", bp::raw_function(&BlobVec_add_blob));
  bp::class_<vector<Blob<Dtype>*> >("RawBlobVec")
    .def(bp::vector_indexing_suite<vector<Blob<Dtype>*>, true>());
  bp::class_<vector<shared_ptr<Layer<Dtype> > > >("LayerVec")
    .def(bp::vector_indexing_suite<vector<shared_ptr<Layer<Dtype> > >, true>());

  register_vector_type<string>("StringVec");
  register_vector_type<int>("IntVec");
  register_vector_type<float>("FloatVec");
  register_vector_type<double>("DoubleVec");
  register_vector_type<bool>("BoolVec");

  bp::class_<vector<shared_ptr<Net<Dtype> > > >("NetVec")
    .def(bp::vector_indexing_suite<vector<shared_ptr<Net<Dtype> > >, true>());

  bp::class_<NCCL<Dtype>, shared_ptr<NCCL<Dtype> >,
    boost::noncopyable>("NCCL",
                        bp::init<shared_ptr<Solver<Dtype> >, const string&>())
#ifdef USE_NCCL
    .def("new_uid", NCCL_New_Uid).staticmethod("new_uid")
    .def("bcast", &NCCL<Dtype>::Broadcast)
#endif
    /* NOLINT_NEXT_LINE(whitespace/semicolon) */
  ;
  BP_REGISTER_SHARED_PTR_TO_PYTHON(NCCL<Dtype>);

  bp::class_<Timer, shared_ptr<Timer>, boost::noncopyable>(
    "Timer", bp::init<>())
    .def("start", &Timer::Start)
    .def("stop", &Timer::Stop)
    .add_property("ms", &Timer::MilliSeconds);
  BP_REGISTER_SHARED_PTR_TO_PYTHON(Timer);

  // boost python expects a void (missing) return value, while import_array
  // returns NULL for python3. import_array1() forces a void return value.
  import_array1();
}

}  // namespace caffe
