#pragma once

#include <memory>
#include <nanobind/nanobind.h>

#include "action/action.hpp"
#include "metadata/context.hpp"
#include "random.hpp"
#include "sql_variant/generic.hpp"

namespace nb = nanobind;

namespace pyaction_detail {

// worker threads are plain std::threads, never attached to the interpreter.
// touching a python object's refcount from such a thread without a guard is
// unsafe, so the final decref (when the last copy of the shared_ptr below
// goes away) always happens under an acquired gil.
inline void release_under_gil(nb::object *obj) {
  if (!Py_IsInitialized()) {
    // last reference dropped after interpreter shutdown (e.g. a python
    // action registered into a C++ static registry, destroyed after
    // Py_Finalize): acquiring a thread state now is UB. deliberately leak
    // - leaking at shutdown is safe, crashing is not.
    return;
  }
  nb::gil_scoped_acquire guard;
  delete obj;
}

} // namespace pyaction_detail

// wraps a python callable so copying it around (action factories get copied
// per worker loop iteration) is a plain atomic shared_ptr bump, never a
// python refcount op on some random unattached thread. only construction
// (always from a thread already holding the gil, since it comes from a
// python call) and destruction (guarded above) touch the interpreter outside
// of execute().
using py_fn_ptr = std::shared_ptr<nb::object>;

inline py_fn_ptr make_py_action_fn(nb::object fn) {
  return py_fn_ptr(new nb::object(std::move(fn)),
                   pyaction_detail::release_under_gil);
}

// C++ action wrapping a python callable. worker threads call execute()
// concurrently; on free-threaded python this runs truly parallel.
class PyCallableAction : public action::Action {
public:
  explicit PyCallableAction(py_fn_ptr fn) : fn(std::move(fn)) {}

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override {
    nb::gil_scoped_acquire guard;
    try {
      (*fn)(nb::cast(&metaCtx, nb::rv_policy::reference),
            nb::cast(&rand, nb::rv_policy::reference),
            nb::cast(connection, nb::rv_policy::reference));
    } catch (nb::python_error &e) {
      throw action::ActionException("python_action", e.what());
    }
  }

private:
  py_fn_ptr fn;
};
