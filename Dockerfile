FROM ubuntu:20.04
LABEL maintainer="qiqiustc@gmail.com"
LABEL version="1.2"
LABEL description="FrameX!"
ARG DEBIAN_FRONTEND=noninteractive

RUN apt update
RUN apt install -y libboost-all-dev cmake g++ gcc libtbb-dev 
RUN apt install -y libgl1-mesa-dev \
    libglu1-mesa-dev \
    libegl1-mesa-dev \
    libgles2-mesa-dev \
    mesa-utils \
    x11-apps \
    vim \
    git
RUN apt install -y python3 python3-pip

RUN apt install -y ninja-build
RUN apt install -y libboost-filesystem-dev libboost-system-dev libboost-program-options-dev libsuitesparse-dev
RUN apt install -y libgl1-mesa-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
RUN apt install -y python3-pip npm
RUN npm install npm@latest -g


RUN pip install "jax[cpu]==0.3.25"
RUN pip install gurobipy==10.0.2
RUN pip install pybullet_planning>=0.6.0
RUN pip install termcolor networkx==3.1 matplotlib==3.7.1



## download knitro from google drive and unzip
RUN pip install gdown
RUN gdown 1flOWpmtuiropee6XrBirxF0i0q1gLsUP
RUN tar -xf knitro-13.1.0-Linux-64.tar.xz
ENV KNITRODIR=/knitro-13.1.0-Linux-64
ENV LD_LIBRARY_PATH=/knitro-13.1.0-Linux-64/lib
RUN cd /knitro-13.1.0-Linux-64/examples/Python && python3 setup.py instal
