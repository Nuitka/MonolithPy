import sys
import os
import shutil
import glob
import sysconfig
import __np__
import mkembeddata
import distutils
import distutils.ccompiler

embed_path = None
out_lib_path = None

def main():
    with __np__.TemporaryDirectory() as tmpdir:
        mkembeddata.base_path = embed_path
        mkembeddata.out_dir = tmpdir
        mkembeddata.main()

        compiler = distutils.ccompiler.new_compiler(verbose=5)
        try:
            compiler.initialize()
        except AttributeError:
            pass

        objs = compiler.compile(
            [os.path.join(tmpdir, "np_embed_data.c")], output_dir=tmpdir
        )

        compiler.create_static_lib([*objs,
             os.path.join(sysconfig.get_config_var("prefix"), "Embedded", "np_embed" + compiler.obj_extension)],
                                   "result_lib", output_dir=tmpdir)

        shutil.copy(glob.glob(os.path.join(tmpdir, "*result_lib.*"))[0], out_lib_path)


if __name__ == "__main__":
    embed_path = os.path.abspath(sys.argv[1])
    out_lib_path = os.path.abspath(sys.argv[2])
    main()
