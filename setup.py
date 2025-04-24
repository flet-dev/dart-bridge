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
    name="dart-bridge",
    version="0.2.0",
    description="A Python C extension for interacting with the Dart SDK from Python",
    long_description=open("README.md").read(),
    long_description_content_type="text/markdown",
    author="Flet Team",
    author_email="hello@flet.dev",
    url="https://github.com/flet-dev/dart-bridge",
    project_urls={
        "Homepage": "https://github.com/flet-dev/dart-bridge",
        "Issues": "https://github.com/flet-dev/dart-bridge/issues",
    },
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: C",
        "Programming Language :: Cython",
        "Operating System :: OS Independent",
        "Topic :: Software Development :: Libraries :: Python Modules",
    ],
    keywords="dart ffi bridge cython",
    python_requires=">=3.10",
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
