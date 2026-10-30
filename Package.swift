// swift-tools-version:5.9
// Vladiktron Spoke: the Mac app (Swift, app/) and the virtual board it runs (C, src/).
// Unicorn is built first by CMake (build.sh step 1) into build/unicorn; `swift build` then links it statically.
// build.sh wraps the result into "build/Vladiktron Spoke.app" and signs it. Xcode can open this package to edit
// and debug the app.
import PackageDescription

let unicorn = Context.packageDirectory + "/build/unicorn/libunicorn.a"

let package = Package(
    name: "VladiktronSpoke",
    platforms: [.macOS(.v14)],
    targets: [
        .target(
            name: "MCBoard",
            path: "src",
            sources: ["mcboard.c", "mcfirmware.c"],
            publicHeadersPath: "include",
            cSettings: [.headerSearchPath("../vendor/unicorn/include"), .unsafeFlags(["-O2"])]
        ),
        .executableTarget(
            name: "VladiktronSpoke",
            dependencies: ["MCBoard"],
            path: "app",
            exclude: ["Resources"],
            linkerSettings: [.unsafeFlags([unicorn]), .linkedLibrary("z")]
        ),
        .testTarget(                                  // `swift test`: the MiniLab mapping (tests/swift)
            name: "VladiktronSpokeTests",
            dependencies: ["VladiktronSpoke"],
            path: "tests/swift",
            linkerSettings: [.unsafeFlags([unicorn]), .linkedLibrary("z")]
        ),
    ]
)
