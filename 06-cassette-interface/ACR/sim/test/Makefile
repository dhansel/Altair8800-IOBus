test: MITS KCS CUTS

MITS: .PHONY
	@echo -e "\n-------- Testing MITS format:"
	../ACR.exe -q -o test.tap -C 4kBasic32MITS.tap 4kBasic32MITS.wav
	../ACR.exe -q -o test.tap -C 4kBasic40KCS.tap 4kBasic40KCS.wav
	../ACR.exe -q -o test.wav test.tap
	../ACR.exe -q -c test.tap test.wav
	rm -f test.wav test.tap

KCS: .PHONY
	@echo -e "\n-------- Testing KCS format:"
	../ACR.exe -q -f KCS -o test.tap -C 4kBasic40KCS.tap 4kBasic40KCS.wav
	../ACR.exe -q -f KCS -o test.wav test.tap
	../ACR.exe -q -f KCS -c test.tap test.wav
	rm -f test.wav test.tap

CUTS: .PHONY
	@echo -e "\n-------- Testing CUTS format:"
	../ACR.exe -q -f CUTS -o test.tap -C PTBasicCUTS.tap PTBasicCUTS.wav
	../ACR.exe -q -f CUTS -o test.wav test.tap
	../ACR.exe -q -f CUTS -c test.tap test.wav
	rm -f test.wav test.tap

.PHONY: