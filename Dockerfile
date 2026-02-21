FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    qt5-qmake \
    qtbase5-dev \
    qttools5-dev \
    libqt5widgets5 \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy project files
COPY . .

# Build the project
RUN qmake macsfancontrol.pro && make

# Default command - run the app (won't work without SMC/hwmon, but shows it builds)
CMD ["./macsfancontrol"]
