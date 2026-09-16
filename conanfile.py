from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class OrderBooksConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    test_requires = "gtest/1.18.0"
    default_options = {
        "gtest/*:build_gmock": False,
    }

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def layout(self):
        cmake_layout(self)
