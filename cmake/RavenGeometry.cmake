# Raven's dialogs, tooltips, scroll thumbs and focus outlines add radii even when
# theme radii are zero. Compile narrowly adapted copies in the build directory
# so the application's square geometry also covers built-in dialogs. Keep the
# bundled Raven sources and notices intact; use the same adaptation on both OSes.
function(ccurate_square_raven_geometry)
 get_target_property(raven_dir raven SOURCE_DIR)
 get_target_property(raven_sources raven SOURCES)
 set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/ccurate-raven-geometry")
 file(MAKE_DIRECTORY "${generated_dir}")
 foreach(source widgets/common widgets/text_input widget/window)
  get_filename_component(name "${source}" NAME)
  set(original "${raven_dir}/src/${source}.c")
  file(READ "${original}" content)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
  if(name STREQUAL "common")
   set(expressions "look.radius + 1.0f" "look.radius + away" "look.radius + width"
                   "(thickness - 4.0f) * 0.5f")
  elseif(name STREQUAL "text_input")
   set(expressions "rvn_widget_metric(widget, R_METRIC_CORNER_RADIUS) + away")
  else()
   set(expressions "10.0f, 28.0f, RVN_THEME_COLOR_OF(theme, R_COLOR_SHADOW)"
                   "10.0f, RVN_THEME_COLOR_OF(theme, R_COLOR_POPUP)"
                   "4.0f, 8.0f, RVN_THEME_COLOR_OF(theme, R_COLOR_SHADOW)"
                   "4.0f, RVN_THEME_COLOR_OF(theme, R_COLOR_TOOLTIP)")
  endif()
  foreach(expression IN LISTS expressions)
   string(FIND "${content}" "${expression}" position)
   if(position EQUAL -1)
    message(FATAL_ERROR "Raven ${name}.c changed: review CCurate's square geometry adaptation")
   endif()
   if(name STREQUAL "window")
    string(REGEX REPLACE "^(10|4)\\.0f" "RVN_THEME_METRIC_OF(theme, R_METRIC_POPUP_RADIUS)"
           replacement "${expression}")
   else()
    set(replacement "(rvn_widget_metric(widget, R_METRIC_CORNER_RADIUS) > 0.0f ? (${expression}) : 0.0f)")
   endif()
   string(REPLACE "${expression}" "${replacement}" content "${content}")
  endforeach()
  set(generated "${generated_dir}/${name}.c")
  file(CONFIGURE OUTPUT "${generated}" CONTENT "${content}" @ONLY)
  list(REMOVE_ITEM raven_sources "src/${source}.c" "${original}")
  list(APPEND raven_sources "${generated}")
 endforeach()
 set_property(TARGET raven PROPERTY SOURCES "${raven_sources}")
 target_include_directories(raven PRIVATE "${raven_dir}/src/widgets" "${raven_dir}/src/widget")
endfunction()

ccurate_square_raven_geometry()
