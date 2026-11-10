# NshvcGame.cmake: one call turns a game's sources into a package for the
# platform being built.
#
#   nshvc_game(mygame SOURCES src/a.c src/b.c ASSETS data SHADERS shaders ICON icon.png TITLE "My Game")
function(nshvc_game target)
  cmake_parse_arguments(G "" "ASSETS;SHADERS;ICON;TITLE" "SOURCES" ${ARGN})
  if(NOT G_TITLE)
    set(G_TITLE ${target})
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
    if(G_ICON)
      target_sources(${target} PRIVATE ${G_ICON})
      set_source_files_properties(${G_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()
  else()
    set(_where "$<TARGET_FILE_DIR:${target}>")
    if(WIN32)
      set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
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
