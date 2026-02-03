# jit.decklink

A Max/MSP Jitter external to send video matrices to Blackmagic Design DeckLink devices.

## Features

*   **Device Discovery**: Automatically scans for available DeckLink devices.
*   **Dynamic Output**: Populates a `umenu` with device names.
*   **Video Output**: Sends `jit_matrix` content to the selected device.

## Prerequisites

*   **Cycling '74 Max SDK**: Included via `min-api`.
*   **Min-API**: Modern C++ API for Max (included as submodule).
*   **Blackmagic DeckLink SDK**: You must download this from the Blackmagic Design website.

## Setup & Build

1.  **Clone the repository**:
    ```bash
    git clone --recursive https://github.com/your/jit.decklink.git
    cd jit.decklink
    ```

2.  **Download DeckLink SDK**:
    *   Go to [Blackmagic Design Developer Support](https://www.blackmagicdesign.com/developer/).
    *   Download the **Desktop Video SDK**.
    *   Extract the SDK.
    *   Copy the `DeckLinkAPI.h` (and other necessary headers if any, usually just one for the API definition, but platform specific headers might be needed) into the `include/` directory of this project.
    *   **Mac/Linux**: You might need `DeckLinkAPIDispatch.cpp` from the SDK to be added to the source, or link against the framework. This project assumes you add the necessary implementation files or link against the libraries provided by the SDK.
    *   **Windows**: Link against `DeckLinkAPI.lib`.

3.  **Build**:
    ```bash
    mkdir build
    cd build
    cmake ..
    cmake --build .
    ```

4.  **Install**:
    *   Copy the resulting external (e.g., `jit.decklink.mxo` or `.mxe64`) to your Max packages or externals folder.

## Usage

1.  Create a `jit.decklink` object in Max.
2.  Connect a `umenu` to the right-most outlet.
3.  Send the `scan` message to the object to populate the menu.
4.  Select a device from the menu (the object listens to `int` or you can use the `device` attribute).
5.  Send a `jit_matrix` (e.g., from `jit.movie` or `jit.grab`) to the left inlet.

## Attributes

*   `device`: Index of the DeckLink device to use (default: 0).

## Messages

*   `scan`: Refresh the list of available devices.
*   `jit_matrix`: Output the received matrix video frame.
