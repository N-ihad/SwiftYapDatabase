// swift-tools-version:5.3
import PackageDescription

let package = Package(
    name: "Swift-YapDatabase",
    products: [
        .library(
            name: "YapDatabase",
            targets: ["YapDatabase"]
        ),
        .library(
            name: "SwiftYapDatabase",
            targets: ["SwiftYapDatabase"]
        )
    ],
    dependencies: [
        .package(url: "https://github.com/sqlcipher/sqlcipher.git", exact: "4.5.5"),
    ],
    targets: [
        .target(
            name: "YapDatabase",
            dependencies: [
                .product(name: "sqlcipher", package: "sqlcipher")
            ],
            cSettings: [
                .headerSearchPath("privateInclude"),
                .define("SQLITE_HAS_CODEC")
            ]
        ),
        .target(
            name: "SwiftYapDatabase",
            dependencies: ["YapDatabase"]
        ),
        .testTarget(
            name: "SwiftYapDatabaseTests",
            dependencies: ["SwiftYapDatabase"]
        ),
    ]
)
