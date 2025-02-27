# **TF2 SDK – Source SDK 2013 Refactor**

This repository contains the **refactored and optimized** version of the **Source SDK 2013**, focussing on **Team Fortress 2 (TF2)** and removing the code for other games.

🚀 **Our goal is to clean up, modernize, and improve the maintainability of the TF2 SDK while keeping it performant and stable.**

## **✨ What's New in This Refactor?**

- **Updated & Modernized Codebase** – Removing deprecated code and improving readability.
- **TF2-Specific Improvements** – Stripping out unrelated HL2/Portal code to keep the SDK focused on TF2.
- **Bug Fixes & Stability Enhancements** – Based on known issues from Valve and the community.
- **Performance Benchmarks** – Tracking optimizations to ensure smooth gameplay.
- **Better Dependency Management** – Making third-party library updates easier.
- **Unit Testing & Debugging Tools** – Laying the foundation for more structured development.

---

## **🛠️ Build Instructions**

### **Clone the Repository**

Use the following command to download the repository:

```sh
git clone https://github.com/DeWolfRobin/TF2CodeRefactor
```

---

### **💻 Windows Setup**

#### **Requirements:**

- **Source SDK 2013 Multiplayer** (installed via Steam)
- **Visual Studio 2022**

#### **Steps:**

1. Inside the cloned directory, navigate to the `src` folder and run:

   ```bat
   createallprojects.bat
   ```

   This generates the **Visual Studio project** (`everything.sln`), which will be used to build your mod.

2. Open **Visual Studio 2022**, then:
   - Go to **`Build > Build Solution`** and wait for the compilation to complete.
   - Select the **`Client (TF)`** project you wish to run.
   - Right-click and choose **`Set as Startup Project`**.
   - Click the **▶ Local Windows Debugger** button in the toolbar to launch your mod.

🚀 **Default launch options** are already set for the `Release` configuration.

---

### **🐧 Linux Setup**

#### **Requirements:**

- **Source SDK 2013 Multiplayer** (installed via Steam)
- **Podman** (containerized build environment)

#### **Steps:**

1. Inside the cloned directory, navigate to the `src` folder and run:

   ```bash
   ./buildallprojects
   ```

   This will **automatically build all necessary projects** against the **Steam Runtime**.

2. To run your mod, navigate to the `game` folder and execute:
   ```bash
   ./mod_tf
   ```
   ✅ **Mods distributed on Steam MUST be built against the Steam Runtime** – these steps ensure compliance.

---

## **📦 Distributing Your Mod**

If you plan to release your mod, Valve provides guidance on both **Steam and non-Steam distribution**:  
📖 **[Distributing Source Engine Mods](https://partner.steamgames.com/doc/sdk/uploading/distributing_source_engine)**

---

## **📚 Additional Resources**

- 🔹 **[Valve Developer Wiki](https://developer.valvesoftware.com/wiki/Source_SDK_2013)**
- 🔹 **TF2 SDK Bug Tracker & Known Issues (GitHub Issues)**
- 🔹 **TF2 SDK Performance Benchmarks & Optimization Logs**

🚀 **Want to contribute?** Check out our **[Contribution Guide](CONTRIBUTING.md)** for details on how to help!

---

## **📜 License**

The SDK is licensed under the **[SOURCE 1 SDK LICENSE](LICENSE)**, allowing non-commercial use. See the **[LICENSE](LICENSE)** file in the root of the repository for full details.

For more information, refer to **[Distributing Your Mod](#-distributing-your-mod)**.

---

💡 **This project aims to make TF2 development more accessible and maintainable. Join us in improving the SDK!** 🚀
