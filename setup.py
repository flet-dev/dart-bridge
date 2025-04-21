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
    name="dart-bridge",
    version="0.1.0",
    description="A Cython bridge for interacting with the Dart SDK from Python",
    ext_modules=cythonize(ext_modules, language_level=3),
    zip_safe=False,
)
