from setuptools import Extension, setup
from wheel.bdist_wheel import bdist_wheel


class bdist_wheel_abi3(bdist_wheel):
    def get_tag(self):
        python, abi, plat = super().get_tag()

        if python.startswith("cp"):
            # on CPython, our wheels are abi3 and compatible back to 3.10
            return "cp310", "abi3", plat

        return python, abi, plat


setup(
    ext_modules=[
        Extension(
            "dart_bridge",
            sources=["src/dart_bridge.c", "src/dart_api/dart_api_dl.c"],
            include_dirs=["src/dart_api"],
            define_macros=[("Py_LIMITED_API", "0x030A0000")],  # Target 3.10
            py_limited_api=True,
        )
    ],
    cmdclass={"bdist_wheel": bdist_wheel_abi3},
)
