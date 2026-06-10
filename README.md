# Master’s Thesis Project

This repository contains the full development work for my Master’s thesis in Electrical Engineering.

## Project Overview

The project consists of two tightly integrated parts:

* A **3D-printed force sensor** based on conductive PLA and strain-sensitive structures.
* A **custom PCB-based embedded system** built around the **EFR32MG22E microcontroller**, responsible for signal acquisition, processing, and wireless communication.

The system communicates via **Zigbee to a Home Assistant installation**, where sensor data is collected, processed, and visualized in a smart-home environment.

## Contents

* `/firmware`
  Embedded firmware for the EFR32MG22E (developed in C using Visual Studio Code with the Silicon Labs Simplicity Studio extension and SDK).

* `/hardware`
  PCB design files, schematics, and related electronics documentation.

* `/sensor`
  3D-printed sensor designs, mechanical structures, and material-related experiments (conductive PLA).

* `/docs`
  Overleaf LaTeX project files for the written thesis, including figures, tables, and report structure.

## Key Technologies

* EFR32MG22E microcontroller
* Zigbee wireless communication
* Home Assistant integration
* Conductive PLA 3D printing
* PCB design and analog front-end electronics
* Embedded C firmware (Simplicity Studio SDK)
* LaTeX (Overleaf)

## Project Status

This project is currently in progress. The work is in an advanced stage, focused on implementation, debugging, validation, and final thesis writing. The submission deadline is **2 July**.

## Purpose

This repository is used for version control, structured development, and centralized documentation of both hardware and software components of the thesis.
