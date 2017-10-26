import subprocess
import sys

from setuptools import setup, Command

class CustomInstall(Command):

    user_options = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        protoc_command = ['make', 'compiler']
        if subprocess.call(protoc_command) != 0:
            sys.exit(-1)

        protoc_command = ['make', 'compiler-api']
        if subprocess.call(protoc_command) != 0:
            sys.exit(-1)


setup(
    name='yask',
    version='v2-alpha',
    description='YASK--Yet Another Stencil Kernel: '
    'A framework to facilitate exploration of the HPC '
    'stencil-performance design space',
    url='https://01.org/yask',
    author='Intel Corporation',
    license='MIT',
    packages = ['yask'],
    cmdclass={
        'build_ext': CustomInstall,
    }
)
