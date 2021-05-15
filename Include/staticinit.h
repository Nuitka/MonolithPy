#ifndef Py_STATICINIT_H
#define Py_STATICINIT_H

#include "object.h"
#include "import.h"

#define NUITKA_PYTHON_STATIC

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN64) || defined(_WIN32)
    extern  PyObject* PyInit_unicodedata(void);
    extern  PyObject* PyInit_select(void);
    extern  PyObject* PyInit__bz2(void);
    extern  PyObject* PyInit__lzma(void);
    extern  PyObject* PyInit__hashlib(void);
    extern  PyObject* PyInit_pyexpat(void);
    extern  PyObject* PyInit__socket(void);
    extern  PyObject* PyInit__ssl(void);
    extern  PyObject* PyInit__queue(void);
    extern  PyObject* PyInit__multiprocessing(void);
    extern  PyObject* PyInit__decimal(void);
    extern  PyObject* PyInit__ctypes(void);
    extern  PyObject* PyInit__asyncio(void);
    extern  PyObject* PyInit__overlapped(void);
    extern  PyObject* PyInit__elementtree(void);
    extern  PyObject* PyInit__uuid(void);
    extern  PyObject* PyInit__sqlite3(void);
    extern  PyObject* PyInit__tkinter(void);
    extern  PyObject* PyInit__zoneinfo(void);
#endif

#ifdef __cplusplus
}
#endif // __cplusplus

static inline void Py_InitStaticModules() {

#if defined(_WIN64) || defined(_WIN32)
    PyImport_AppendInittab("unicodedata", PyInit_unicodedata);
    PyImport_AppendInittab("select", PyInit_select);
    PyImport_AppendInittab("_bz2", PyInit__bz2);
    PyImport_AppendInittab("_lzma", PyInit__lzma);
    PyImport_AppendInittab("_hashlib", PyInit__hashlib);
    PyImport_AppendInittab("pyexpat", PyInit_pyexpat);
    PyImport_AppendInittab("_socket", PyInit__socket);
    PyImport_AppendInittab("_ssl", PyInit__ssl);
    PyImport_AppendInittab("_queue", PyInit__queue);
    PyImport_AppendInittab("_multiprocessing", PyInit__multiprocessing);
    PyImport_AppendInittab("_decimal", PyInit__decimal);
    PyImport_AppendInittab("_ctypes", PyInit__ctypes);
    PyImport_AppendInittab("_asyncio", PyInit__asyncio);
    PyImport_AppendInittab("_overlapped", PyInit__overlapped);
    PyImport_AppendInittab("_elementtree", PyInit__elementtree);
    PyImport_AppendInittab("_uuid", PyInit__uuid);
    PyImport_AppendInittab("_sqlite3", PyInit__sqlite3);
    PyImport_AppendInittab("_tkinter", PyInit__tkinter);
    PyImport_AppendInittab("_zoneinfo", PyInit__zoneinfo);
#endif

}

#endif

#endif // !Py_STATICINIT_H
