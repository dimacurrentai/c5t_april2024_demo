#pragma once

// TODO(dkorolev): All these are next steps.
//
// The wrapper over native Current's `dlib` support. Extensions:
// 1) Handles startup and tear down, both stateful and stateless. Namely, will call, if present:
//    - `OnC5TLoad()`.
//    - `OnC5TUnload()`.
//    - ...
// 2) Renames and does the symlinks magic as the `.so` file changes on disk and is requested to be reloaded.
//    The issue is that by default the environment, at least on Linux, will not even attempt to re-load the same lib.
// 3) Exposes the generic `.Call()` inteface, as well as the `CallOrDefault()` one.
//    TODO(dkorolev): Make them cleaner and document them better.
// 4) Offers the generic `IGeneric` and `IGeneric::Use<T>()` means to call other stuff.
//    The code defined in the `dlib_*` can receive implementations of many interfaces at once.
//    At the very least, the logger and the lifetime managers come in handy.
//
// Usage:
// 1) Create an interface provider as `struct IMyInterface : virtual IDlib`.
// 2) Have the `dlib`-exposed function be some `extern "C" void MyDLibExternalFunction(IDLib& dlib);`
// 3) In that function, `dlib.Use<void(IMyInterface&)>(...)`, see the `demo_*` in (some) C5T repo for details.
// 4) Call it with an instance of your `IMyInterface`, see the `demo_*` in (some) C5T repo for details.

#include <functional>
#include <memory>
#include <string>
#include <type_traits>

#include "typesystem/optional.h"
#include "typesystem/helpers.h"  // IWYU pragma: keep

// The thinnest possible wrapper over a loaded `dlib`.
class C5T_DLib {
 protected:
  virtual void* GetRawPF(std::string const& fn_name) = 0;

 public:
  virtual ~C5T_DLib() = default;

  bool Has(std::string const& fn_name) { return GetRawPF(fn_name) != nullptr; }

  template <typename F,
            typename... ARGS,
            typename T_RETVAL = std::invoke_result_t<F, ARGS...>,
            class = typename std::enable_if<!std::is_same_v<T_RETVAL, void>>::type>
  T_RETVAL CallOrDefault(std::string const& fn_name, ARGS&&... args) {
    auto const pf = reinterpret_cast<F*>(GetRawPF(fn_name));
    if (pf) {
      return (*pf)(std::forward<ARGS>(args)...);
    } else {
      return T_RETVAL();
    }
  }

  template <typename F,
            typename... ARGS,
            typename T_RETVAL = std::invoke_result_t<F, ARGS...>,
            class = typename std::enable_if<std::is_same_v<T_RETVAL, void>>::type>
  void CallOrDefault(std::string const& fn_name, ARGS&&... args) {
    auto const pf = reinterpret_cast<F*>(GetRawPF(fn_name));
    if (pf) {
      (*pf)(std::forward<ARGS>(args)...);
    }
  }

  template <typename F,
            typename... ARGS,
            typename T_RETVAL = std::invoke_result_t<F, ARGS...>,
            class = typename std::enable_if<!std::is_same_v<T_RETVAL, void>>::type>
  Optional<T_RETVAL> Call(std::string const& fn_name, ARGS&&... args) {
    auto const pf = reinterpret_cast<F*>(GetRawPF(fn_name));
    if (pf) {
      return (*pf)(std::forward<ARGS>(args)...);
    } else {
      return nullptr;
    }
  }

  template <typename F,
            typename... ARGS,
            typename T_RETVAL = std::invoke_result_t<F, ARGS...>,
            class = typename std::enable_if<std::is_same_v<T_RETVAL, void>>::type>
  void Call(std::string const& fn_name, ARGS&&... args) {
    auto const pf = reinterpret_cast<F*>(GetRawPF(fn_name));
    if (pf) {
      (*pf)(std::forward<ARGS>(args)...);
    }
  }
};

// The "meta-interface" to pass interfaces (impl object instances by references) to and from dlib-s.
struct IDLib {
 protected:
  virtual ~IDLib() = default;

 public:
  template <
      class I,
      class F,
      class = typename std::enable_if<std::is_same_v<decltype(std::declval<F>()(*std::declval<I*>())), void>>::type>
  void Use(F&& f) {
    if (I* i = dynamic_cast<I*>(this)) {
      f(*i);
    }
  }

  template <
      class I,
      class F,
      class = typename std::enable_if<!std::is_same_v<decltype(std::declval<F>()(*std::declval<I*>())), void>>::type>
  Optional<decltype(std::declval<F>()(*std::declval<I*>()))> Use(F&& f) {
    if (I* i = dynamic_cast<I*>(this)) {
      return f(*i);
    } else {
      return nullptr;
    }
  }

  template <class I, class F, class G = std::function<decltype(std::declval<F>()(*std::declval<I*>()))()>>
  decltype(std::declval<F>()(*std::declval<I*>())) UseOrDefault(
      F&& f, G&& g = []() -> decltype(std::declval<F>()(*std::declval<I*>())) {
        return decltype(std::declval<F>()(std::declval<I&>()))();
      }) {
    if (I* i = dynamic_cast<I*>(this)) {
      return f(*i);
    } else {
      return g();
    }
  }
};

// The public interface, to enable `C5T_LIFETIME_MANAGER`-level injection.
enum class C5T_DLIB_RELOAD_STATUS : int { InternalError, Fail, Loaded, UpToDate, Reloaded };
struct C5T_DLIB_RELOAD_RESULT final {
  C5T_DLIB_RELOAD_STATUS res = C5T_DLIB_RELOAD_STATUS::InternalError;
  C5T_DLib* ptr = nullptr;
};

class C5T_DLibs_Manager_Interface {
 public:
  ~C5T_DLibs_Manager_Interface() = default;
  virtual void SetBaseDir(std::string s) = 0;
  virtual C5T_DLIB_RELOAD_RESULT LoadLibAndReloadAsNeededFromLockedSection(std::string const& name) = 0;
  virtual bool UseDLib(std::string const& name,
                       std::function<void(C5T_DLib&)> cb_success,
                       std::function<void()> cb_fail) = 0;
  virtual void ListDLibs(std::function<void(std::string)> f) = 0;
  virtual C5T_DLIB_RELOAD_RESULT DoLoadOrReloadDLib(std::string const& name) = 0;
};

// Initialize, tell the `DLIB` framework which dir to load dynamic libraries from.
void C5T_DLIB_SET_BASE_DIR(std::string base_dir);

// Use this instead to leverage `C5T_LIFETIME_MANAGER` and have all the `.so`-s properly unloaded during shutdown.
// Template parameter to not have this code result in compilation error.
// void C5T_LIFETIME_MANAGER_DLIB_SET_BASE_DIR(std::string base_dir);
std::unique_ptr<C5T_DLibs_Manager_Interface> INTERNAL_C5T_DLIB_CREATE_SCOPED_INSTANCE();
void C5T_DLIB_USE_PROVIDED_INSTANCE_AND_SET_BASE_DIR(C5T_DLibs_Manager_Interface& instance, std::string base_dir);

// List the currently loaded `DLIB`-s, for debugging purposes.
void C5T_DLIB_LIST(std::function<void(std::string)>);

// Use the `DLIB`, load it if needed, re-load it if needed.
// TODO(dkorolev): DLib lifetime management, this is the next step.
bool C5T_DLIB_USE(
    std::string const& lib_name, std::function<void(C5T_DLib&)>, std::function<void()> = [] {});

// Reloads the lib as needed, with the "symlink trick" so that the library is truly re-loaded.
C5T_DLIB_RELOAD_RESULT C5T_DLIB_RELOAD(std::string const& lib_name);
