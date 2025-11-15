install(
    TARGETS OpenStudioRecorder_exe
    RUNTIME COMPONENT OpenStudioRecorder_Runtime
)

if(PROJECT_IS_TOP_LEVEL)
  include(CPack)
endif()
