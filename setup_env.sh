#!/bin/bash

echo "================================================="
echo "   BoWWServer (x86_64) - Environment Setup       "
echo "================================================="

# 1. Update package lists
echo -e "\n[1/4] Updating apt package lists..."
sudo apt update

# 2. Install required C++ libraries and build tools
echo -e "\n[2/4] Installing dependencies..."
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libboost-system-dev \
    libboost-thread-dev \
    nlohmann-json3-dev \
    libyaml-cpp-dev \
    libasound2-dev \
    libavahi-client-dev \
    libavahi-common-dev

# 3. Fix ALSA hardware permissions for the current user
echo -e "\n[3/4] Fixing ALSA Audio Permissions..."
if groups $USER | grep &>/dev/null '\baudio\b'; then
    echo "User '$USER' is already in the 'audio' group."
else
    sudo usermod -aG audio $USER
    echo "Added '$USER' to the 'audio' group."
    echo "WARNING: You must log out and log back in (or run 'newgrp audio') for this to take effect!"
fi

# 4. Configure Persistent ALSA Loopback
echo -e "\n[4/4] Configuring Persistent ALSA Loopback Device..."
if grep -q "^snd-aloop$" /etc/modules; then
    echo "snd-aloop is already configured to load on boot."
else
    echo "snd-aloop" | sudo tee -a /etc/modules > /dev/null
    echo "Added snd-aloop to /etc/modules for permanent boot loading."
fi

# Load it immediately for the current session so a reboot isn't strictly required right now
sudo modprobe snd-aloop
echo "ALSA Loopback module loaded for the current session."

echo -e "\n================================================="
echo "Setup Complete!"
echo "You can now compile the server by running:"
echo "  mkdir build && cd build && cmake .. && make -j4"
echo "================================================="
