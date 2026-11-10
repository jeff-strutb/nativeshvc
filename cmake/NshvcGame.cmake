# NshvcGame.cmake: one call turns a game's sources into a package for the
# platform being built.
#
#   nshvc_game(mygame SOURCES src/a.c src/b.c ASSETS data SHADERS shaders ICON icon.png TITLE "My Game")
#
# ICON is one square PNG; tools/nshvc-icons makes each platform's form of it
# when it changes (macOS an .icns, iOS an asset catalog, Windows an .ico
# linked in as icon 1, Linux a PNG and the window's icon beside the program,
# Android the launcher's).
find_program(NSHVC_PYTHON NAMES python3 python)

function(nshvc_game target)
  cmake_parse_arguments(G "" "ASSETS;SHADERS;ICON;TITLE" "SOURCES" ${ARGN})
  if(NOT G_TITLE)
    set(G_TITLE ${target})
  endif()
  set(_icons_tool ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/nshvc-icons)
  set(_icons ${CMAKE_CURRENT_BINARY_DIR}/${target}-icon)
  if(G_ICON)
    get_filename_component(G_ICON ${G_ICON} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT NSHVC_PYTHON)
      message(FATAL_ERROR "nshvc_game: ICON needs Python 3 (tools/nshvc-icons)")
    endif()
  endif()
  if(ANDROID)
    # what tools/nshvc-build packages: apk/ holds the manifest, the library, assets and resources
    add_library(${target} SHARED ${G_SOURCES})
    set(_apk ${CMAKE_BINARY_DIR}/apk)
    set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${_apk}/lib/${ANDROID_ABI})
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _package ${target})
    set(_icon "")
    if(G_ICON)
      file(MAKE_DIRECTORY ${_apk}/res/mipmap)
      configure_file(${G_ICON} ${_apk}/res/mipmap/icon.png COPYONLY)
      set(_icon "android:icon=\"@mipmap/icon\" ")
    endif()
    file(WRITE ${_apk}/AndroidManifest.xml
"<?xml version=\"1.0\" encoding=\"utf-8\"?>
<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"com.nativeshvc.${_package}\" android:versionCode=\"1\" android:versionName=\"1.0\">
  <uses-sdk android:minSdkVersion=\"26\" android:targetSdkVersion=\"35\"/>
  <uses-feature android:name=\"android.hardware.vulkan.version\" android:version=\"0x400000\" android:required=\"true\"/>
  <application android:label=\"${G_TITLE}\" ${_icon}android:hasCode=\"false\" android:extractNativeLibs=\"true\">
    <activity android:name=\"android.app.NativeActivity\" android:exported=\"true\" android:theme=\"@android:style/Theme.NoTitleBar.Fullscreen\" android:configChanges=\"orientation|screenSize|screenLayout|keyboard|keyboardHidden|navigation|uiMode\">
      <meta-data android:name=\"android.app.lib_name\" android:value=\"${target}\"/>
      <intent-filter>
        <action android:name=\"android.intent.action.MAIN\"/>
        <category android:name=\"android.intent.category.LAUNCHER\"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
")
  else()
    add_executable(${target} ${G_SOURCES})
  endif()
  target_link_libraries(${target} PRIVATE nshvc)
  if(APPLE)
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_BUNDLE_NAME "${G_TITLE}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.nativeshvc.${target}")
    set(_where "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources")
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
      set(_where "$<TARGET_BUNDLE_DIR:${target}>")
    endif()
    if(G_ICON AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
      # an asset catalog, named in Info.plist: CMake's own Info.plist with CFBundleIcons added
      set(_sdk iphoneos)
      if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(_sdk iphonesimulator)
      endif()
      add_custom_command(OUTPUT ${_icons}/Assets.car
        COMMAND ${NSHVC_PYTHON} ${_icons_tool} ${G_ICON} ${_icons} ios ${_sdk}
        DEPENDS ${G_ICON} ${_icons_tool} VERBATIM)
      target_sources(${target} PRIVATE ${_icons}/Assets.car)
      set_source_files_properties(${_icons}/Assets.car PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
      file(READ ${CMAKE_ROOT}/Modules/MacOSXBundleInfo.plist.in _plist)
      set(_named "<dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconName</key><string>AppIcon</string></dict></dict>")
      string(REGEX REPLACE "</dict>[ \t\r\n]*</plist>[ \t\r\n]*$"
        "\t<key>CFBundleIcons</key>${_named}\n\t<key>CFBundleIcons~ipad</key>${_named}\n</dict>\n</plist>\n" _plist "${_plist}")
      if(NOT _plist MATCHES "CFBundleIcons~ipad")
        message(FATAL_ERROR "nshvc_game: could not add the icon to CMake's Info.plist template")
      endif()
      file(WRITE ${_icons}/Info.plist.in "${_plist}")
      set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${_icons}/Info.plist.in)
    elseif(G_ICON)
      add_custom_command(OUTPUT ${_icons}/AppIcon.icns
        COMMAND ${NSHVC_PYTHON} ${_icons_tool} ${G_ICON} ${_icons} macos
        DEPENDS ${G_ICON} ${_icons_tool} VERBATIM)
      target_sources(${target} PRIVATE ${_icons}/AppIcon.icns)
      set_source_files_properties(${_icons}/AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
      set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE AppIcon)
    endif()
  else()
    set(_where "$<TARGET_FILE_DIR:${target}>")
    if(WIN32)
      set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
      if(G_ICON)
        # icon 1, which the window class loads
        add_custom_command(OUTPUT ${_icons}/icon.rc ${_icons}/icon.ico
          COMMAND ${NSHVC_PYTHON} ${_icons_tool} ${G_ICON} ${_icons} windows
          DEPENDS ${G_ICON} ${_icons_tool} VERBATIM)
        target_sources(${target} PRIVATE ${_icons}/icon.rc)
        set_source_files_properties(${_icons}/icon.rc PROPERTIES OBJECT_DEPENDS ${_icons}/icon.ico)
      endif()
    elseif(G_ICON AND NOT ANDROID)
      # beside the program: icon.png for a desktop file, icon.argb the window's own
      add_custom_command(OUTPUT ${_icons}/icon.png ${_icons}/icon.argb
        COMMAND ${NSHVC_PYTHON} ${_icons_tool} ${G_ICON} ${_icons} linux
        DEPENDS ${G_ICON} ${_icons_tool} VERBATIM)
      add_custom_target(${target}_icon ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_icons}/icon.png ${_icons}/icon.argb $<TARGET_FILE_DIR:${target}>
        DEPENDS ${_icons}/icon.png ${_icons}/icon.argb VERBATIM)
    endif()
  endif()
  if(ANDROID)
    set(_where ${CMAKE_BINARY_DIR}/apk/assets)
  endif()
  if(G_ASSETS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/${G_ASSETS} ${_where})
  endif()
  if(G_SHADERS)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/nshvc-shaders ${CMAKE_CURRENT_SOURCE_DIR}/${G_SHADERS} ${_where}/shaders ${NSHVC_RENDER})
  endif()
endfunction()
