from setuptools import Extension, setup

setup(
    name="dart_bridge",
    version="0.1.0",
    ext_modules=[
        Extension(
            "dart_bridge",
            sources=["src/dart_bridge.c", "src/dart_api/dart_api_dl.c"],
            include_dirs=["src/dart_api"],
        )
    ],
)
