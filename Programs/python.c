/* Minimal main program -- everything is loaded from the library */

#include "Python.h"


#ifdef MS_WINDOWS
int
wmain(int argc, wchar_t **argv)
{
    if (argc && argv)
        Py_SetProgramName(argv[0]);

    Py_InitStaticModules();

    return Py_Main(argc, argv);
}
#else
int
main(int argc, char **argv)
{
    Py_InitStaticModules();
    return Py_BytesMain(argc, argv);
}
#endif
