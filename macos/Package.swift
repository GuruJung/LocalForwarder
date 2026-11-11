// swift-tools-version: 5.9
import PackageDescription
let package = Package(
    name: "LocalForwarder",
    platforms: [.macOS("15.0")],
    products: [.executable(name: "LocalForwarder", targets: ["LocalForwarder"]),
               .library(name: "LocalForwarderCore", targets: ["LocalForwarderCore"])],
    targets: [.target(name: "LocalForwarderCore"),
              .executableTarget(name: "LocalForwarder", dependencies: ["LocalForwarderCore"]),
              .testTarget(name: "LocalForwarderCoreTests", dependencies: ["LocalForwarderCore"])])
