compile:
	arduino-cli compile .

upload:
	arduino-cli upload .

monitor:
	arduino-cli monitor -c baudrate=115200

update-clang:
	arduino-cli compile --only-compilation-database --build-path ./build .
