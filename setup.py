from Cython.Build import cythonize
from setuptools import Extension, setup

ext_modules = [
    Extension(
        name="dart_bridge",
        sources=["src/dart_bridge.pyx"],
        include_dirs=["src/dart_api"],
    )
]

setup(
    name="dart_bridge",
    ext_modules=cythonize(ext_modules),
    zip_safe=False,
)
