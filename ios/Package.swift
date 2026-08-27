// swift-tools-version: 5.9

// Local Swift package for the iOS build of the openDAQ Bluetooth module.
//
// The four .xcframework bundles this package vends are BUILD OUTPUT, not sources: run
// ./build-xcframeworks.sh before resolving it. They are deliberately not committed -- each is tens
// of megabytes, and all of it is reproducible from the CMake build.
//
// The binary targets use local paths rather than .binaryTarget(url:checksum:) because this
// repository publishes no release artefacts; it does not publish the Android AAR either. If that
// changes, zip each bundle, run `swift package compute-checksum <name>.xcframework.zip`, and swap
// the four targets over. Nothing else in this manifest has to move.
//
// Living under ios/ rather than at the repository root mirrors android/, and carries a limitation
// worth stating: SwiftPM resolves a *URL* dependency only from a manifest at the repository root,
// so this is consumable as a local package -- .package(path:), or dragged into Xcode -- not by URL.
//
// The package vends no Swift API. openDAQ's headers are C++ and cannot be imported into Swift, so
// an application talks to the SDK from its own Objective-C++ layer. What the package does is get
// all four dylibs embedded and signed into App.app/Frameworks with their filenames intact --
// libbluetooth_module.module.dylib above all, because openDAQ's ModuleManager discovers modules by
// scanning for that suffix and nothing else.

import PackageDescription

let package = Package(
    name: "OpenDAQBluetooth",
    platforms: [
        // Matches --deployment-target in build-xcframeworks.sh; keep the two in step.
        .iOS(.v15)
    ],
    products: [
        // One product pulling in all four. The application never references the module
        // symbolically -- openDAQ dlopens it at runtime -- so leaving it out of an explicit target
        // list is an easy mistake, and it shows up only as "no devices found".
        .library(
            name: "OpenDAQBluetooth",
            targets: [
                "OpenDAQCoreTypes",
                "OpenDAQCoreObjects",
                "OpenDAQ",
                "OpenDAQBluetoothModule",
            ]
        )
    ],
    targets: [
        .binaryTarget(name: "OpenDAQCoreTypes",
                      path: "xcframeworks/OpenDAQCoreTypes.xcframework"),
        .binaryTarget(name: "OpenDAQCoreObjects",
                      path: "xcframeworks/OpenDAQCoreObjects.xcframework"),
        .binaryTarget(name: "OpenDAQ",
                      path: "xcframeworks/OpenDAQ.xcframework"),
        .binaryTarget(name: "OpenDAQBluetoothModule",
                      path: "xcframeworks/OpenDAQBluetoothModule.xcframework"),
    ]
)
