// TODO: - Implement other datatypes for ndarray

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <Python.h>
#include <structmember.h>
#define PY_ARRAY_UNIQUE_SYMBOL RAYLIB_ARRAY_API
#include <numpy/arrayobject.h>
#include <iostream>

#include "types.pb.h"
#include "worker.h"
#include "utils.h"

RayConfig global_ray_config;

extern "C" {

static int PyObjectToWorker(PyObject* object, Worker **worker);

// Object references

typedef struct {
  PyObject_HEAD
  ObjectID id;
  // We give the PyObjectID object a reference to the worker capsule object to
  // make sure that the worker capsule does not go out of scope until all of the
  // object references have gone out of scope. The reason for this is that the
  // worker capsule destructor destroys the worker object. If the worker object
  // has been destroyed, then when the object reference tries to call
  // worker->decrement_reference_count, we can get a segfault.
  PyObject* worker_capsule;
} PyObjectID;

static void PyObjectID_dealloc(PyObjectID *self) {
  Worker* worker;
  PyObjectToWorker(self->worker_capsule, &worker);
  std::vector<ObjectID> objectids;
  objectids.push_back(self->id);
  RAY_LOG(RAY_REFCOUNT, "In PyObjectID_dealloc, calling decrement_reference_count for objectid " << self->id);
  worker->decrement_reference_count(objectids);
  Py_DECREF(self->worker_capsule); // The corresponding increment happens in PyObjectID_init.
  self->ob_type->tp_free((PyObject*) self);
}

static PyObject* PyObjectID_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  PyObjectID* self = (PyObjectID*) type->tp_alloc(type, 0);
  if (self != NULL) {
    self->id = 0;
  }
  return (PyObject*) self;
}

static int PyObjectID_init(PyObjectID *self, PyObject *args, PyObject *kwds) {
  if (!PyArg_ParseTuple(args, "iO", &self->id, &self->worker_capsule)) {
    return -1;
  }
  Worker* worker;
  PyObjectToWorker(self->worker_capsule, &worker);
  Py_INCREF(self->worker_capsule); // The corresponding decrement happens in PyObjectID_dealloc.
  std::vector<ObjectID> objectids;
  objectids.push_back(self->id);
  RAY_LOG(RAY_REFCOUNT, "In PyObjectID_init, calling increment_reference_count for objectid " << objectids[0]);
  worker->increment_reference_count(objectids);
  return 0;
};

static int PyObjectID_compare(PyObject* a, PyObject* b) {
  PyObjectID* A = (PyObjectID*) a;
  PyObjectID* B = (PyObjectID*) b;
  if (A->id < B->id) {
    return -1;
  }
  if (A->id > B->id) {
    return 1;
  }
  return 0;
}

static long PyObjectID_hash(PyObject* a) {
  PyObjectID* A = (PyObjectID*) a;
  PyObject* tuple = PyTuple_New(1);
  PyTuple_SetItem(tuple, 0, PyInt_FromLong(A->id));
  long hash = PyObject_Hash(tuple);
  Py_XDECREF(tuple);
  return hash;
}

char RAY_ID_LITERAL[] = "id";
char RAY_OBJECT_ID_LITERAL[] = "object id";

static PyMemberDef PyObjectID_members[] = {
  {RAY_ID_LITERAL, T_INT, offsetof(PyObjectID, id), 0, RAY_OBJECT_ID_LITERAL},
  {NULL}
};

static PyTypeObject PyObjectIDType = {
  PyObject_HEAD_INIT(NULL)
  0,                         /* ob_size */
  "ray.ObjectID",            /* tp_name */
  sizeof(PyObjectID),        /* tp_basicsize */
  0,                         /* tp_itemsize */
  (destructor)PyObjectID_dealloc,          /* tp_dealloc */
  0,                         /* tp_print */
  0,                         /* tp_getattr */
  0,                         /* tp_setattr */
  PyObjectID_compare,        /* tp_compare */
  0,                         /* tp_repr */
  0,                         /* tp_as_number */
  0,                         /* tp_as_sequence */
  0,                         /* tp_as_mapping */
  PyObjectID_hash,           /* tp_hash */
  0,                         /* tp_call */
  0,                         /* tp_str */
  0,                         /* tp_getattro */
  0,                         /* tp_setattro */
  0,                         /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT,        /* tp_flags */
  "Ray objects",             /* tp_doc */
  0,                         /* tp_traverse */
  0,                         /* tp_clear */
  0,                         /* tp_richcompare */
  0,                         /* tp_weaklistoffset */
  0,                         /* tp_iter */
  0,                         /* tp_iternext */
  0,                         /* tp_methods */
  PyObjectID_members,        /* tp_members */
  0,                         /* tp_getset */
  0,                         /* tp_base */
  0,                         /* tp_dict */
  0,                         /* tp_descr_get */
  0,                         /* tp_descr_set */
  0,                         /* tp_dictoffset */
  (initproc)PyObjectID_init, /* tp_init */
  0,                         /* tp_alloc */
  PyObjectID_new,            /* tp_new */
};

// create PyObjectID from C++ (could be made more efficient if neccessary)
PyObject* make_pyobjectid(PyObject* worker_capsule, ObjectID objectid) {
  PyObject* arglist = Py_BuildValue("(iO)", objectid, worker_capsule);
  PyObject* result = PyObject_CallObject((PyObject*) &PyObjectIDType, arglist);
  Py_DECREF(arglist);
  return result;
}

// Error handling

static PyObject *RayError;
static PyObject *RaySizeError;

// Pass arguments from Python to C++

static int PyObjectToTask(PyObject* object, Task **task) {
  if (PyCapsule_IsValid(object, "task")) {
    *task = static_cast<Task*>(PyCapsule_GetPointer(object, "task"));
    return 1;
  } else {
    PyErr_SetString(PyExc_TypeError, "must be a 'task' capsule");
    return 0;
  }
}

static int PyObjectToObj(PyObject* object, Obj **obj) {
  if (PyCapsule_IsValid(object, "obj")) {
    *obj = static_cast<Obj*>(PyCapsule_GetPointer(object, "obj"));
    return 1;
  } else {
    PyErr_SetString(PyExc_TypeError, "must be a 'obj' capsule");
    return 0;
  }
}

static int PyObjectToWorker(PyObject* object, Worker **worker) {
  if (PyCapsule_IsValid(object, "worker")) {
    *worker = static_cast<Worker*>(PyCapsule_GetPointer(object, "worker"));
    return 1;
  } else {
    PyErr_SetString(PyExc_TypeError, "must be a 'worker' capsule");
    return 0;
  }
}

static int PyObjectToObjectID(PyObject* object, ObjectID *objectid) {
  if (PyObject_IsInstance(object, (PyObject*)&PyObjectIDType)) {
    *objectid = ((PyObjectID*) object)->id;
    return 1;
  } else {
    PyErr_SetString(PyExc_TypeError, "must be an object reference");
    return 0;
  }
}

// Destructors

static void ObjCapsule_Destructor(PyObject* capsule) {
  Obj* obj = static_cast<Obj*>(PyCapsule_GetPointer(capsule, "obj"));
  delete obj;
}

static void WorkerCapsule_Destructor(PyObject* capsule) {
  Worker* obj = static_cast<Worker*>(PyCapsule_GetPointer(capsule, "worker"));
  delete obj;
}

static void TaskCapsule_Destructor(PyObject* capsule) {
  Task* obj = static_cast<Task*>(PyCapsule_GetPointer(capsule, "task"));
  delete obj;
}

// Helper methods

// Pass ownership of both the key and the value to the PyDict.
// This is only required for PyDicts, not for PyLists or PyTuples, compare
// https://docs.python.org/2/c-api/dict.html
// https://docs.python.org/2/c-api/list.html
// https://docs.python.org/2/c-api/tuple.html

void set_dict_item_and_transfer_ownership(PyObject* dict, PyObject* key, PyObject* val) {
  PyDict_SetItem(dict, key, val);
  Py_XDECREF(key);
  Py_XDECREF(val);
}

// This converts an Python ObjectID to an Python integer.
static PyObject* serialize_objectid(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  if (!PyArg_ParseTuple(args, "O&O&", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid)) {
    return NULL;
  }
  return PyInt_FromLong(objectid);
}

// This converts a Python integer to a Python ObjectID.
static PyObject* deserialize_objectid(PyObject* self, PyObject* args) {
  PyObject* worker_capsule;
  int objectid;
  if (!PyArg_ParseTuple(args, "Oi", &worker_capsule, &objectid)) {
    return NULL;
  }
  return make_pyobjectid(worker_capsule, static_cast<ObjectID>(objectid));
}

static PyObject* allocate_buffer(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  SegmentId segmentid;
  long size;
  if (!PyArg_ParseTuple(args, "O&O&l", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid, &size)) {
    return NULL;
  }
  void* address = reinterpret_cast<void*>(const_cast<char*>(worker->allocate_buffer(objectid, size, segmentid)));
  std::vector<npy_intp> dim({size});
  PyObject* t = PyTuple_New(2);
  PyTuple_SetItem(t, 0, PyArray_SimpleNewFromData(1, dim.data(), NPY_BYTE, address));
  PyTuple_SetItem(t, 1, PyInt_FromLong(segmentid));
  return t;
}

static PyObject* finish_buffer(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  long segmentid;
  long metadata_offset;
  if (!PyArg_ParseTuple(args, "O&O&ll", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid, &segmentid, &metadata_offset)) {
    return NULL;
  }
  return worker->finish_buffer(objectid, segmentid, metadata_offset);
}

static PyObject* get_buffer(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  int64_t size;
  SegmentId segmentid;
  int64_t metadata_offset;
  if (!PyArg_ParseTuple(args, "O&O&", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid)) {
    return NULL;
  }
  void* address = reinterpret_cast<void*>(const_cast<char*>(worker->get_buffer(objectid, size, segmentid, metadata_offset)));
  std::vector<npy_intp> dim({static_cast<npy_intp>(size)});
  PyObject* t = PyTuple_New(3);
  PyTuple_SetItem(t, 0, PyArray_SimpleNewFromData(1, dim.data(), NPY_BYTE, address));
  PyTuple_SetItem(t, 1, PyInt_FromLong(segmentid));
  PyTuple_SetItem(t, 2, PyInt_FromLong(metadata_offset));
  return t;
}

static PyObject* is_arrow(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  if (!PyArg_ParseTuple(args, "O&O&", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid)) {
    return NULL;
  }
  if (worker->is_arrow(objectid))
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

static PyObject* unmap_object(PyObject* self, PyObject* args) {
  Worker* worker;
  int segmentid;
  if (!PyArg_ParseTuple(args, "O&i", &PyObjectToWorker, &worker, &segmentid)) {
    return NULL;
  }
  worker->unmap_object(segmentid);
  Py_RETURN_NONE;
}

static PyObject* serialize_task(PyObject* self, PyObject* args) {
  PyObject* worker_capsule;
  Task* task = new Task(); // TODO: to be freed in capsule destructor
  char* name;
  int len;
  PyObject* arguments;
  if (!PyArg_ParseTuple(args, "Os#O", &worker_capsule, &name, &len, &arguments)) {
    return NULL;
  }
  task->set_name(std::string(name, len));
  std::vector<ObjectID> objectids; // This is a vector of all the objectids that are serialized in this task, including objectids that are contained in Python objects that are passed by value.
  if (PyList_Check(arguments)) {
    for (size_t i = 0, size = PyList_Size(arguments); i < size; ++i) {
      PyObject* element = PyList_GetItem(arguments, i);
      if (PyObject_IsInstance(element, (PyObject*)&PyObjectIDType)) {
        // Handle the case where the argument to the task is an ObjectID.
        ObjectID objectid = ((PyObjectID*) element)->id;
        task->add_arg()->set_objectid(objectid);
        objectids.push_back(objectid);
      } else if (PyString_CheckExact(element)) {
        // Handle the case where the argument to the task is being passed by
        // value and we receive an argument serialized as a string here.
        char* buffer;
        Py_ssize_t length;
        PyString_AsStringAndSize(element, &buffer, &length);
        task->add_arg()->set_serialized_arg(std::string(buffer, length));
      } else {
        RAY_CHECK(false, "This code should be unreachable.");
      }
    }
  } else {
    PyErr_SetString(RayError, "serialize_task: second argument needs to be a list");
    return NULL;
  }
  Worker* worker;
  PyObjectToWorker(worker_capsule, &worker);
  if (objectids.size() > 0) {
    RAY_LOG(RAY_REFCOUNT, "In serialize_task, calling increment_reference_count for contained objectids");
    worker->increment_reference_count(objectids);
  }
  std::string output;
  task->SerializeToString(&output);
  int task_size = output.length();
  return PyCapsule_New(static_cast<void*>(task), "task", &TaskCapsule_Destructor);
}

static PyObject* deserialize_task(PyObject* worker_capsule, const Task& task) {
  std::vector<ObjectID> objectids; // This is a vector of all the objectids that were serialized in this task, including objectids that are contained in Python objects that are passed by value.
  PyObject* string = PyString_FromStringAndSize(task.name().c_str(), task.name().size());
  int argsize = task.arg_size();
  PyObject* arglist = PyList_New(argsize);
  for (int i = 0; i < argsize; ++i) {
    if (task.arg(i).serialized_arg().empty()) {
      PyList_SetItem(arglist, i, make_pyobjectid(worker_capsule, task.arg(i).objectid()));
      objectids.push_back(task.arg(i).objectid());
    } else {
      PyObject* serialized_arg = PyString_FromStringAndSize(task.arg(i).serialized_arg().data(), task.arg(i).serialized_arg().size());
      PyList_SetItem(arglist, i, serialized_arg);
    }
  }
  Worker* worker;
  PyObjectToWorker(worker_capsule, &worker);
  worker->decrement_reference_count(objectids);
  int resultsize = task.result_size();
  std::vector<ObjectID> result_objectids;
  PyObject* resultlist = PyList_New(resultsize);
  for (int i = 0; i < resultsize; ++i) {
    PyList_SetItem(resultlist, i, make_pyobjectid(worker_capsule, task.result(i)));
    result_objectids.push_back(task.result(i));
  }
  worker->decrement_reference_count(result_objectids); // The corresponding increment is done in SubmitTask in the scheduler.
  PyObject* t = PyTuple_New(3); // We set the items of the tuple using PyTuple_SetItem, because that transfers ownership to the tuple.
  PyTuple_SetItem(t, 0, string);
  PyTuple_SetItem(t, 1, arglist);
  PyTuple_SetItem(t, 2, resultlist);
  return t;
}

// Ray Python API

static PyObject* create_worker(PyObject* self, PyObject* args) {
  const char* node_ip_address;
  const char* scheduler_address;
  // The object store address can be the empty string, in which case the
  // scheduler will choose the object store address.
  const char* objstore_address;
  int mode;
  const char* log_file_name;
  if (!PyArg_ParseTuple(args, "sssis", &node_ip_address, &scheduler_address, &objstore_address, &mode, &log_file_name)) {
    return NULL;
  }
  // Set the logging file.
  create_log_dir_or_die(log_file_name);
  global_ray_config.log_to_file = true;
  global_ray_config.logfile.open(log_file_name);
  // Create the worker.
  bool is_driver = (mode != Mode::WORKER_MODE);
  Worker* worker = new Worker(std::string(node_ip_address), std::string(scheduler_address), static_cast<Mode>(mode));
  // Register the worker.
  worker->register_worker(std::string(node_ip_address), std::string(objstore_address), is_driver);

  PyObject* t = PyTuple_New(2);
  PyObject* worker_capsule = PyCapsule_New(static_cast<void*>(worker), "worker", &WorkerCapsule_Destructor);
  PyTuple_SetItem(t, 0, worker_capsule);
  PyTuple_SetItem(t, 1, PyString_FromString(worker->get_worker_address()));
  return t;
}

static PyObject* disconnect(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  worker->disconnect();
  Py_RETURN_NONE;
}

static PyObject* connected(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  if (worker->connected()) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject* wait_for_next_message(PyObject* self, PyObject* args) {
  PyObject* worker_capsule;
  if (!PyArg_ParseTuple(args, "O", &worker_capsule)) {
    return NULL;
  }
  Worker* worker;
  PyObjectToWorker(worker_capsule, &worker);
  if (std::unique_ptr<WorkerMessage> message = worker->receive_next_message()) {
    bool task_present = !message->task().name().empty();
    bool function_present = !message->function().implementation().empty();
    bool reusable_variable_present = !message->reusable_variable().name().empty();
    bool function_to_run_present = !message->function_to_run().implementation().empty();
    RAY_CHECK(task_present + function_present + reusable_variable_present + function_to_run_present <= 1, "The worker message should contain at most one item.");
    PyObject* t = PyTuple_New(2);
    if (task_present) {
      PyTuple_SetItem(t, 0, PyString_FromString("task"));
      PyTuple_SetItem(t, 1, deserialize_task(worker_capsule, message->task()));
    } else if (function_present) {
      PyTuple_SetItem(t, 0, PyString_FromString("function"));
      PyObject* remote_function_data = PyTuple_New(2);
      PyTuple_SetItem(remote_function_data, 0, PyString_FromStringAndSize(message->function().name().data(), static_cast<ssize_t>(message->function().name().size())));
      PyTuple_SetItem(remote_function_data, 1, PyString_FromStringAndSize(message->function().implementation().data(), static_cast<ssize_t>(message->function().implementation().size())));
      PyTuple_SetItem(t, 1, remote_function_data);
    } else if (reusable_variable_present) {
      PyTuple_SetItem(t, 0, PyString_FromString("reusable_variable"));
      PyObject* reusable_variable = PyTuple_New(3);
      PyTuple_SetItem(reusable_variable, 0, PyString_FromStringAndSize(message->reusable_variable().name().data(), static_cast<ssize_t>(message->reusable_variable().name().size())));
      PyTuple_SetItem(reusable_variable, 1, PyString_FromStringAndSize(message->reusable_variable().initializer().implementation().data(), static_cast<ssize_t>(message->reusable_variable().initializer().implementation().size())));
      PyTuple_SetItem(reusable_variable, 2, PyString_FromStringAndSize(message->reusable_variable().reinitializer().implementation().data(), static_cast<ssize_t>(message->reusable_variable().reinitializer().implementation().size())));
      PyTuple_SetItem(t, 1, reusable_variable);
    } else if (function_to_run_present) {
      PyTuple_SetItem(t, 0, PyString_FromString("function_to_run"));
      PyTuple_SetItem(t, 1, PyString_FromStringAndSize(message->function_to_run().implementation().data(), static_cast<ssize_t>(message->function_to_run().implementation().size())));
    } else {
      PyTuple_SetItem(t, 0, PyString_FromString("die"));
      Py_INCREF(Py_None);
      PyTuple_SetItem(t, 1, Py_None);
    }
    return t;
  }
  RAY_CHECK(false, "This code should be unreachable.");
  Py_RETURN_NONE;
}

static PyObject* run_function_on_all_workers(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* function;
  int function_size;
  if (!PyArg_ParseTuple(args, "O&s#", &PyObjectToWorker, &worker, &function, &function_size)) {
    return NULL;
  }
  worker->run_function_on_all_workers(std::string(function, static_cast<size_t>(function_size)));
  Py_RETURN_NONE;
}

static PyObject* export_remote_function(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* function_name;
  const char* function;
  int function_size;
  if (!PyArg_ParseTuple(args, "O&ss#", &PyObjectToWorker, &worker, &function_name, &function, &function_size)) {
    return NULL;
  }
  if (worker->export_remote_function(std::string(function_name), std::string(function, static_cast<size_t>(function_size)))) {
    Py_RETURN_TRUE;
  } else {
    Py_RETURN_FALSE;
  }
}

static PyObject* export_reusable_variable(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* name;
  int name_size;
  const char* initializer;
  int initializer_size;
  const char* reinitializer;
  int reinitializer_size;
  if (!PyArg_ParseTuple(args, "O&s#s#s#", &PyObjectToWorker, &worker, &name, &name_size, &initializer, &initializer_size, &reinitializer, &reinitializer_size)) {
    return NULL;
  }
  std::string name_str(name, static_cast<size_t>(name_size));
  std::string initializer_str(initializer, static_cast<size_t>(initializer_size));
  std::string reinitializer_str(reinitializer, static_cast<size_t>(reinitializer_size));
  worker->export_reusable_variable(name_str, initializer_str, reinitializer_str);
  Py_RETURN_NONE;
}

static PyObject* submit_task(PyObject* self, PyObject* args) {
  PyObject* worker_capsule;
  Task* task;
  if (!PyArg_ParseTuple(args, "OO&", &worker_capsule, &PyObjectToTask, &task)) {
    return NULL;
  }
  Worker* worker;
  PyObjectToWorker(worker_capsule, &worker);
  SubmitTaskRequest request;
  request.set_allocated_task(task);
  SubmitTaskReply reply = worker->submit_task(&request);
  request.release_task(); // TODO: Make sure that task is not moved, otherwise capsule pointer needs to be updated
  if (reply.no_workers()) {
    PyErr_SetString(RayError, "No workers have registered with the scheduler, so this function cannot be run.");
    return NULL;
  }
  if (!reply.function_registered()) {
    PyErr_SetString(RayError, "No worker has registered this function with the scheduler.");
    return NULL;
  }
  int size = reply.result_size();
  PyObject* list = PyList_New(size);
  std::vector<ObjectID> result_objectids;
  for (int i = 0; i < size; ++i) {
    PyList_SetItem(list, i, make_pyobjectid(worker_capsule, reply.result(i)));
    result_objectids.push_back(reply.result(i));
  }
  worker->decrement_reference_count(result_objectids); // The corresponding increment is done in SubmitTask in the scheduler.
  return list;
}

static PyObject* ready_for_new_task(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  worker->ready_for_new_task();
  Py_RETURN_NONE;
}

static PyObject* register_remote_function(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* function_name;
  int num_return_vals;
  if (!PyArg_ParseTuple(args, "O&si", &PyObjectToWorker, &worker, &function_name, &num_return_vals)) {
    return NULL;
  }
  worker->register_remote_function(std::string(function_name), num_return_vals);
  Py_RETURN_NONE;
}

static PyObject* notify_failure(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* name;
  const char* error_message;
  int type;
  if (!PyArg_ParseTuple(args, "O&ssi", &PyObjectToWorker, &worker, &name, &error_message, &type)) {
    return NULL;
  }
  worker->notify_failure(static_cast<FailedType>(type), std::string(name), std::string(error_message));
  Py_RETURN_NONE;
}

static PyObject* get_objectid(PyObject* self, PyObject* args) {
  PyObject* worker_capsule;
  if (!PyArg_ParseTuple(args, "O", &worker_capsule)) {
    return NULL;
  }
  Worker* worker;
  PyObjectToWorker(worker_capsule, &worker);
  ObjectID objectid = worker->get_objectid();
  return make_pyobjectid(worker_capsule, objectid);
}

static PyObject* add_contained_objectids(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  PyObject* contained_objectids;
  if (!PyArg_ParseTuple(args, "O&O&O", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid, &contained_objectids)) {
    return NULL;
  }
  RAY_CHECK(PyList_Check(contained_objectids), "The contained_objectids argument must be a list.")
  std::vector<ObjectID> vec_contained_objectids;
  size_t size = PyList_Size(contained_objectids);
  for (size_t i = 0; i < size; ++i) {
    ObjectID contained_objectid;
    PyObjectToObjectID(PyList_GetItem(contained_objectids, i), &contained_objectid);
    vec_contained_objectids.push_back(contained_objectid);
  }
  worker->add_contained_objectids(objectid, vec_contained_objectids);
  Py_RETURN_NONE;
}

static PyObject* request_object(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID objectid;
  if (!PyArg_ParseTuple(args, "O&O&", &PyObjectToWorker, &worker, &PyObjectToObjectID, &objectid)) {
    return NULL;
  }
  worker->request_object(objectid);
  Py_RETURN_NONE;
}

static PyObject* wait(PyObject* self, PyObject* args) {
  Worker* worker;
  PyObject* objectids;
  if (!PyArg_ParseTuple(args, "O&O", &PyObjectToWorker, &worker, &objectids)) {
    return NULL;
  }
  std::vector<ObjectID> objectids_vec;
  for (size_t i = 0; i < PyList_Size(objectids); ++i) {
    ObjectID objectid;
    PyObjectToObjectID(PyList_GetItem(objectids, i), &objectid);
    objectids_vec.push_back(objectid);
  }
  std::vector<int> indices = worker->wait(objectids_vec);
  PyObject* result = PyList_New(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    PyList_SetItem(result, i, PyInt_FromLong(indices[i]));
  }
  return result;
}

static PyObject* alias_objectids(PyObject* self, PyObject* args) {
  Worker* worker;
  ObjectID alias_objectid;
  ObjectID target_objectid;
  if (!PyArg_ParseTuple(args, "O&O&O&", &PyObjectToWorker, &worker, &PyObjectToObjectID, &alias_objectid, &PyObjectToObjectID, &target_objectid)) {
    return NULL;
  }
  worker->alias_objectids(alias_objectid, target_objectid);
  Py_RETURN_NONE;
}

static PyObject* scheduler_info(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  ClientContext context;
  SchedulerInfoRequest request;
  SchedulerInfoReply reply;
  worker->scheduler_info(context, request, reply);

  // Unpack the target object reference information.
  PyObject* target_objectid_list = PyList_New(reply.target_objectid_size());
  for (size_t i = 0; i < reply.target_objectid_size(); ++i) {
    PyList_SetItem(target_objectid_list, i, PyInt_FromLong(reply.target_objectid(i)));
  }
  // Unpack the reference count information.
  PyObject* reference_count_list = PyList_New(reply.reference_count_size());
  for (size_t i = 0; i < reply.reference_count_size(); ++i) {
    PyList_SetItem(reference_count_list, i, PyInt_FromLong(reply.reference_count(i)));
  }
  // Unpack the available worker information.
  PyObject* available_worker_list = PyList_New(reply.avail_worker_size());
  for (size_t i = 0; i < reply.avail_worker_size(); ++i) {
    PyList_SetItem(available_worker_list, i, PyInt_FromLong(reply.avail_worker(i)));
  }
  // Unpack the object store information.
  PyObject* objstore_list = PyList_New(reply.objstore_size());
  for (size_t i = 0; i < reply.objstore_size(); ++i) {
    PyObject* objstore_data = PyDict_New();
    set_dict_item_and_transfer_ownership(objstore_data, PyString_FromString("objstoreid"), PyInt_FromLong(reply.objstore(i).objstoreid()));
    set_dict_item_and_transfer_ownership(objstore_data, PyString_FromString("address"), PyString_FromStringAndSize(reply.objstore(i).address().data(), reply.objstore(i).address().size()));
    PyList_SetItem(objstore_list, i, objstore_data);
  }

  // Store the unpacked values in a dictionary to return.
  PyObject* dict = PyDict_New();
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("target_objectids"), target_objectid_list);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("reference_counts"), reference_count_list);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("available_workers"), available_worker_list);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("objstores"), objstore_list);
  return dict;
}

static PyObject* failure_to_dict(const Failure& failure) {
  PyObject* failure_dict = PyDict_New();
  set_dict_item_and_transfer_ownership(failure_dict, PyString_FromString("workerid"), PyInt_FromLong(failure.workerid()));
  set_dict_item_and_transfer_ownership(failure_dict, PyString_FromString("worker_address"), PyString_FromStringAndSize(failure.worker_address().data(), failure.worker_address().size()));
  set_dict_item_and_transfer_ownership(failure_dict, PyString_FromString("function_name"), PyString_FromStringAndSize(failure.name().data(), failure.name().size()));
  set_dict_item_and_transfer_ownership(failure_dict, PyString_FromString("error_message"), PyString_FromStringAndSize(failure.error_message().data(), failure.error_message().size()));
  return failure_dict;
}

static PyObject* task_info(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  ClientContext context;
  TaskInfoRequest request;
  TaskInfoReply reply;
  worker->task_info(context, request, reply);

  PyObject* failed_tasks_list = PyList_New(reply.failed_task_size());
  for (size_t i = 0; i < reply.failed_task_size(); ++i) {
    const TaskStatus& info = reply.failed_task(i);
    PyObject* info_dict = PyDict_New();
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("worker_address"), PyString_FromStringAndSize(info.worker_address().data(), info.worker_address().size()));
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("function_name"), PyString_FromStringAndSize(info.function_name().data(), info.function_name().size()));
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("operationid"), PyInt_FromLong(info.operationid()));
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("error_message"), PyString_FromStringAndSize(info.error_message().data(), info.error_message().size()));
    PyList_SetItem(failed_tasks_list, i, info_dict);
  }

  PyObject* running_tasks_list = PyList_New(reply.running_task_size());
  for (size_t i = 0; i < reply.running_task_size(); ++i) {
    const TaskStatus& info = reply.running_task(i);
    PyObject* info_dict = PyDict_New();
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("worker_address"), PyString_FromStringAndSize(info.worker_address().data(), info.worker_address().size()));
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("function_name"), PyString_FromStringAndSize(info.function_name().data(), info.function_name().size()));
    set_dict_item_and_transfer_ownership(info_dict, PyString_FromString("operationid"), PyInt_FromLong(info.operationid()));
    PyList_SetItem(running_tasks_list, i, info_dict);
  }

  PyObject* failed_remote_function_imports = PyList_New(reply.failed_remote_function_import_size());
  for (size_t i = 0; i < reply.failed_remote_function_import_size(); ++i) {
    PyList_SetItem(failed_remote_function_imports, i, failure_to_dict(reply.failed_remote_function_import(i)));
  }

  PyObject* failed_reusable_variable_imports = PyList_New(reply.failed_reusable_variable_import_size());
  for (size_t i = 0; i < reply.failed_reusable_variable_import_size(); ++i) {
    PyList_SetItem(failed_reusable_variable_imports, i, failure_to_dict(reply.failed_reusable_variable_import(i)));
  }

  PyObject* failed_reinitialize_reusable_variables = PyList_New(reply.failed_reinitialize_reusable_variable_size());
  for (size_t i = 0; i < reply.failed_reinitialize_reusable_variable_size(); ++i) {
    PyList_SetItem(failed_reinitialize_reusable_variables, i, failure_to_dict(reply.failed_reinitialize_reusable_variable(i)));
  }

  PyObject* failed_function_to_runs = PyList_New(reply.failed_function_to_run_size());
  for (size_t i = 0; i < reply.failed_function_to_run_size(); ++i) {
    PyList_SetItem(failed_function_to_runs, i, failure_to_dict(reply.failed_function_to_run(i)));
  }

  PyObject* dict = PyDict_New();
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("failed_tasks"), failed_tasks_list);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("running_tasks"), running_tasks_list);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("failed_remote_function_imports"), failed_remote_function_imports);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("failed_reusable_variable_imports"), failed_reusable_variable_imports);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("failed_reinitialize_reusable_variables"), failed_reinitialize_reusable_variables);
  set_dict_item_and_transfer_ownership(dict, PyString_FromString("failed_function_to_runs"), failed_function_to_runs);
  return dict;
}

static PyObject* dump_computation_graph(PyObject* self, PyObject* args) {
  Worker* worker;
  const char* output_file_name;
  if (!PyArg_ParseTuple(args, "O&s", &PyObjectToWorker, &worker, &output_file_name)) {
    return NULL;
  }
  ClientContext context;
  SchedulerInfoRequest request;
  SchedulerInfoReply reply;
  worker->scheduler_info(context, request, reply);
  std::fstream output(output_file_name, std::ios::out | std::ios::trunc | std::ios::binary);
  RAY_CHECK(reply.computation_graph().SerializeToOstream(&output), "Cannot dump computation graph to file " << output_file_name);
  Py_RETURN_NONE;
}

static PyObject* kill_workers(PyObject* self, PyObject* args) {
  Worker* worker;
  if (!PyArg_ParseTuple(args, "O&", &PyObjectToWorker, &worker)) {
    return NULL;
  }
  ClientContext context;
  if (worker->kill_workers(context)) {
    Py_RETURN_TRUE;
  } else {
    Py_RETURN_FALSE;
  }
}

static PyMethodDef RayLibMethods[] = {
 { "serialize_objectid", serialize_objectid, METH_VARARGS, "serialize an object id" },
 { "deserialize_objectid", deserialize_objectid, METH_VARARGS, "deserialize an object id" },
 { "allocate_buffer", allocate_buffer, METH_VARARGS, "Allocates and returns buffer for objectid."},
 { "finish_buffer", finish_buffer, METH_VARARGS, "Makes the buffer immutable and closes memory segment of objectid."},
 { "get_buffer", get_buffer, METH_VARARGS, "Gets buffer for objectid"},
 { "is_arrow", is_arrow, METH_VARARGS, "is the object in the local object store an arrow object?"},
 { "unmap_object", unmap_object, METH_VARARGS, "unmap the object from the client's shared memory pool"},
 { "serialize_task", serialize_task, METH_VARARGS, "serialize a task to protocol buffers" },
 { "create_worker", create_worker, METH_VARARGS, "connect to the scheduler and the object store" },
 { "disconnect", disconnect, METH_VARARGS, "disconnect the worker from the scheduler and the object store" },
 { "connected", connected, METH_VARARGS, "check if the worker is connected to the scheduler and the object store" },
 { "register_remote_function", register_remote_function, METH_VARARGS, "register a function with the scheduler" },
 { "notify_failure", notify_failure, METH_VARARGS, "notify the scheduler of a failure" },
 { "add_contained_objectids", add_contained_objectids, METH_VARARGS, "notify the scheduler about the object IDs contained in a remote object" },
 { "get_objectid", get_objectid, METH_VARARGS, "register a new object reference with the scheduler" },
 { "request_object" , request_object, METH_VARARGS, "request an object to be delivered to the local object store" },
 { "wait" , wait, METH_VARARGS, "checks the scheduler to see if a object can be gotten" },
 { "alias_objectids", alias_objectids, METH_VARARGS, "make two objectids refer to the same object" },
 { "wait_for_next_message", wait_for_next_message, METH_VARARGS, "get next message from scheduler (blocking)" },
 { "submit_task", submit_task, METH_VARARGS, "call a remote function" },
 { "ready_for_new_task", ready_for_new_task, METH_VARARGS, "notify the scheduler that the worker is ready for a new task" },
 { "scheduler_info", scheduler_info, METH_VARARGS, "get info about scheduler state" },
 { "task_info", task_info, METH_VARARGS, "get information about task statuses and failures" },
 { "run_function_on_all_workers", run_function_on_all_workers, METH_VARARGS, "run an arbitrary function on all workers" },
 { "export_remote_function", export_remote_function, METH_VARARGS, "export a remote function to workers" },
 { "export_reusable_variable", export_reusable_variable, METH_VARARGS, "export a reusable variable to the workers" },
 { "dump_computation_graph", dump_computation_graph, METH_VARARGS, "dump the current computation graph to a file" },
 { "kill_workers", kill_workers, METH_VARARGS, "kills all of the workers" },
 { NULL, NULL, 0, NULL }
};

PyMODINIT_FUNC initlibraylib(void) {
  PyObject* m;
  PyObjectIDType.tp_new = PyType_GenericNew;
  if (PyType_Ready(&PyObjectIDType) < 0) {
    return;
  }
  m = Py_InitModule3("libraylib", RayLibMethods, "Python C Extension for Ray");
  Py_INCREF(&PyObjectIDType);
  PyModule_AddObject(m, "ObjectID", (PyObject *)&PyObjectIDType);
  char ray_error[] = "ray.error";
  char ray_size_error[] = "ray_size.error";
  RayError = PyErr_NewException(ray_error, NULL, NULL);
  RaySizeError = PyErr_NewException(ray_size_error, NULL, NULL);
  Py_INCREF(RayError);
  Py_INCREF(RaySizeError);
  PyModule_AddObject(m, "ray_error", RayError);
  PyModule_AddObject(m, "ray_size_error", RaySizeError);
  import_array();

  // Export constants used for the worker mode types so they can be accessed
  // from Python. The Mode enum is defined in worker.h.
  PyModule_AddIntConstant(m, "SCRIPT_MODE", Mode::SCRIPT_MODE);
  PyModule_AddIntConstant(m, "WORKER_MODE", Mode::WORKER_MODE);
  PyModule_AddIntConstant(m, "PYTHON_MODE", Mode::PYTHON_MODE);
  PyModule_AddIntConstant(m, "SILENT_MODE", Mode::SILENT_MODE);

  // Export constants for the failure types so they can be accessed from Python.
  // The FailedType enum is defined in types.proto.
  PyModule_AddIntConstant(m, "FailedTask", FailedType::FailedTask);
  PyModule_AddIntConstant(m, "FailedRemoteFunctionImport", FailedType::FailedRemoteFunctionImport);
  PyModule_AddIntConstant(m, "FailedReusableVariableImport", FailedType::FailedReusableVariableImport);
  PyModule_AddIntConstant(m, "FailedReinitializeReusableVariable", FailedType::FailedReinitializeReusableVariable);
  PyModule_AddIntConstant(m, "FailedFunctionToRun", FailedType::FailedFunctionToRun);
}

}
