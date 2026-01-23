---
description: how to install PlatformIO Core in WSL
---

To install PlatformIO Core (CLI) in WSL Ubuntu, follow these steps:

1. **Install Prerequisites**:
   Make sure you have Python 3 and its virtual environment module installed.
   ```bash
   sudo apt update
   sudo apt install -y python3 python3-venv curl
   ```

// turbo
2. **Download and Run the Installer**:
   Use the official install script from PlatformIO.
   ```bash
   curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
   python3 get-platformio.py
   ```

3. **Add to PATH**:
   Add the following line to your `~/.bashrc` (or `~/.zshrc`) to make the `pio` command available in your shell:
   ```bash
   export PATH=$PATH:$HOME/.platformio/penv/bin
   ```
   After adding it, reload the config:
   ```bash
   source ~/.bashrc
   ```

4. **Verify Installation**:
   Check if the command works:
   ```bash
   pio --version
   ```
