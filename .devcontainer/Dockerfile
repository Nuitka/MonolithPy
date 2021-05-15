FROM quay.io/pypa/manylinux2014_x86_64

RUN ulimit -n 1024000 && yum install -y perl-IPC-Cmd gperf git

WORKDIR /MonolithPy

# Entry point
CMD ["/bin/bash"]