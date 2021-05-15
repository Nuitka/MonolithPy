/* Minimal main program -- everything is loaded from the library */

#include "Python.h"

extern  PyObject* PyInit__asyncio();
extern  PyObject* PyInit__bz2();
extern  PyObject* PyInit__ctypes();
extern  PyObject* PyInit__decimal();
extern  PyObject* PyInit__elementtree();
extern  PyObject* PyInit__hashlib();
extern  PyObject* PyInit__lzma();
extern  PyObject* PyInit__msi();
extern  PyObject* PyInit__multiprocessing();
extern  PyObject* PyInit__overlapped();
extern  PyObject* PyInit__queue();
extern  PyObject* PyInit__socket();
extern  PyObject* PyInit__sqlite3();
extern  PyObject* PyInit__ssl();
extern  PyObject* PyInit__tkinter();
extern  PyObject* PyInit__uuid();
extern  PyObject* PyInit__zoneinfo();
extern  PyObject* PyInit_pyexpat();
extern  PyObject* PyInit_select();
extern  PyObject* PyInit_unicodedata();
extern  PyObject* PyInit_winsound();

#ifdef MS_WINDOWS
int
wmain(int argc, wchar_t **argv)
{
    if (argc && argv)
        Py_SetProgramName(argv[0]);

    PyImport_AppendInittab("_asyncio", PyInit__asyncio);
    PyImport_AppendInittab("_bz2", PyInit__bz2);
    PyImport_AppendInittab("_ctypes", PyInit__ctypes);
    PyImport_AppendInittab("_decimal", PyInit__decimal);
    PyImport_AppendInittab("_elementtree", PyInit__elementtree);
    PyImport_AppendInittab("_hashlib", PyInit__hashlib);
    PyImport_AppendInittab("_lzma", PyInit__lzma);
    PyImport_AppendInittab("_msi", PyInit__msi);
    PyImport_AppendInittab("_multiprocessing", PyInit__multiprocessing);
    PyImport_AppendInittab("_overlapped", PyInit__overlapped);
    PyImport_AppendInittab("_queue", PyInit__queue);
    PyImport_AppendInittab("_socket", PyInit__socket);
    PyImport_AppendInittab("_sqlite3", PyInit__sqlite3);
    PyImport_AppendInittab("_ssl", PyInit__ssl);
    PyImport_AppendInittab("_tkinter", PyInit__tkinter);
    PyImport_AppendInittab("_uuid", PyInit__uuid);
    PyImport_AppendInittab("_zoneinfo", PyInit__zoneinfo);
    PyImport_AppendInittab("pyexpat", PyInit_pyexpat);
    PyImport_AppendInittab("select", PyInit_select);
    PyImport_AppendInittab("unicodedata", PyInit_unicodedata);
    PyImport_AppendInittab("winsound", PyInit_winsound);

    return Py_Main(argc, argv);
}
#else
int
main(int argc, char **argv)
{
    return Py_BytesMain(argc, argv);
}
#endif
