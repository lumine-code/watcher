{
  "targets": [
    {
      "target_name": "watcher",
      "sources": [
        "src/engine.cc"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")"
      ],
      "defines": [
        "NAPI_VERSION=8",
        "NAPI_CPP_EXCEPTIONS"
      ],
      "cflags!": [
        "-fno-exceptions"
      ],
      "cflags_cc!": [
        "-fno-exceptions",
        "-fno-rtti"
      ],
      "cflags_cc": [
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-missing-field-initializers",
        "-frtti"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "sources": [
              "src/windows.cc"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1,
                "AdditionalOptions": [
                  "/std:c++17",
                  "/guard:cf",
                  "/W3"
                ]
              },
              "VCLinkerTool": {
                "AdditionalOptions": [
                  "/DYNAMICBASE",
                  "/guard:cf"
                ]
              }
            },
            "defines!": [
              "_HAS_EXCEPTIONS=0"
            ],
            "defines": [
              "_HAS_EXCEPTIONS=1"
            ]
          }
        ],
        [
          "OS=='linux'",
          {
            "sources": [
              "src/linux.cc"
            ]
          }
        ],
        [
          "OS=='mac'",
          {
            "sources": [
              "src/macos.cc"
            ],
            "link_settings": {
              "libraries": [
                "CoreServices.framework"
              ]
            },
            "xcode_settings": {
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
              "OTHER_CPLUSPLUSFLAGS": [
                "-fblocks"
              ],
              "GCC_ENABLE_CPP_RTTI": "YES",
              "CLANG_CXX_LIBRARY": "libc++"
            }
          }
        ]
      ]
    }
  ]
}
