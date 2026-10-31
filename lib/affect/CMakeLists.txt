cmake_minimum_required(VERSION 3.20)
project(entelea_affect C)
include(CTest)

add_library(entelea_affect STATIC
	kaleido_drive/src/kaleido_drive.c
	kaleido_emotion/src/kaleido_emotion.c
	kaleido_ignition/src/kaleido_ignition.c)
target_include_directories(entelea_affect PUBLIC
	kaleido_drive/include kaleido_emotion/include kaleido_ignition/include)
target_compile_options(entelea_affect PRIVATE -Wall -Wextra)
target_link_libraries(entelea_affect PUBLIC m)

if(BUILD_TESTING)
	include(FetchContent)
	FetchContent_Declare(unity
		GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
		GIT_TAG cbcd08fa7de711053a3deec6339ee89cad5d2697)
	FetchContent_MakeAvailable(unity)
	add_executable(affect_tests tests/main.c
		kaleido_drive/test/test_kaleido_drive.c
		kaleido_emotion/test/test_kaleido_emotion.c
		kaleido_ignition/test/test_kaleido_ignition.c
		kaleido_ignition/test/test_kaleido_ignition_w3.c)
	target_link_libraries(affect_tests PRIVATE entelea_affect unity)
	add_test(NAME affect_golden COMMAND affect_tests)
endif()
