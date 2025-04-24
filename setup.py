import platform

from setuptools import Extension, setup
from wheel.bdist_wheel import bdist_wheel

# Detect target system
target_system = platform.system()

# Enable ABI3 tag only if not iOS or Android
enable_abi3 = target_system not in ("iOS", "Android")


class bdist_wheel_custom(bdist_wheel):
    def get_tag(self):
        python, abi, plat = super().get_tag()
        if enable_abi3 and python.startswith("cp"):
            return "cp310", "abi3", plat
        return python, abi, plat


# Conditionally set macros and ABI
define_macros = [("Py_LIMITED_API", "0x030A0000")] if enable_abi3 else []
py_limited_api = enable_abi3

setup(
    name="dart_bridge",
    version="0.1.0",
    ext_modules=[
        Extension(
            "dart_bridge",
            sources=["src/dart_bridge.c", "src/dart_api/dart_api_dl.c"],
            include_dirs=["src/dart_api"],
            define_macros=define_macros,
            py_limited_api=py_limited_api,
        )
    ],
    cmdclass={"bdist_wheel": bdist_wheel_custom},
)
