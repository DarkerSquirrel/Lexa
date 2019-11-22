﻿
#include "pch.h"
#include <deque>
#include <iostream>
#include <windows.h>
#include "iat.h"
#include "misc.h"
#include "reloc.h"
#include <regex>
using namespace std;

#include "capstone/capstone.h"
#pragma comment(lib, "capstone_dll.lib")
#include "keystone/keystone.h"
#pragma comment(lib, "keystone.lib")

#pragma warning(disable:4996)

int asmblrOutputBytecode(char* assemblyStr, BYTE* &encode, size_t &size, size_t &count) {
	ks_engine *ks;
	ks_err err;
	err = ks_open(KS_ARCH_X86, KS_MODE_32, &ks);
	if (err != KS_ERR_OK) {
		printf("ERROR: failed on ks_open(), quit\n");
		return -1;
	}

	if (ks_asm(ks, assemblyStr, 0, &encode, &size, &count) != KS_ERR_OK) {
		printf("ERROR: ks_asm() failed & count = %lu, error = %u\n",
			count, ks_errno(ks));
	}

	//ks_free(encode);
	//ks_close(ks);
}

void patchIatImport(BYTE* dumpImg, map<DWORD, string> callViaThunkArr) {

	PIMAGE_SECTION_HEADER selectScnHdr = enumExecSecnHdr(dumpImg)[0]; // assert it's .text
	uint8_t* currPointToBytecode = dumpImg + selectScnHdr->VirtualAddress;
	size_t bytecodeSize = selectScnHdr->SizeOfRawData;

	csh handle; cs_insn *currInsn, *beginCurrInsn; size_t count;
	if (cs_open(CS_ARCH_X86, getNtHdr(dumpImg)->FileHeader.Machine & IMAGE_FILE_MACHINE_I386 ? CS_MODE_32 : CS_MODE_64, &handle) != CS_ERR_OK) return;
	count = cs_disasm(handle, currPointToBytecode, bytecodeSize, getNtHdr(dumpImg)->OptionalHeader.ImageBase + selectScnHdr->VirtualAddress, 0, &currInsn);
	beginCurrInsn = currInsn;

	for (size_t j = count; j-- > 0; currPointToBytecode += currInsn->size, currInsn++) {
		if (strstr(currInsn->op_str, "ptr")) {
			string str_currInsn = string(currInsn->mnemonic) + string(" ") + currInsn->op_str;
			std::cmatch match;
	
			if (regex_match(str_currInsn.c_str(), match, regex("(.+)\x20([^,]+),.dword.ptr.\\[0x(.+)\\]")))
			{
				DWORD callViaVA = -1;
				sscanf(string(match[3]).c_str(), "%x", &callViaVA);
				callViaVA -= getNtHdr(dumpImg)->OptionalHeader.ImageBase;
				map<DWORD, string>::iterator ret = callViaThunkArr.find(callViaVA);
				if (ret == callViaThunkArr.end()) continue;

				string impLibName = ret->second;

				char buf[64];
				PIMAGE_THUNK_DATA callVia = (PIMAGE_THUNK_DATA)(dumpImg + callViaVA);
				PIMAGE_IMPORT_BY_NAME apiName = (PIMAGE_IMPORT_BY_NAME)(dumpImg + callVia->u1.ForwarderString);
				size_t wantedApiAddr = (size_t)GetProcAddress(LoadLibraryA(impLibName.c_str()), apiName->Name);

				BYTE*encode; size_t count; size_t size;
				string newInsn = string(string(match[1]) + " " + string(match[2]) + ", 0x" + string(match[3]));
				asmblrOutputBytecode((char *)newInsn.c_str(), encode, size, count);

				printf("    # patch %p - %s -> %s@%x\n", currPointToBytecode, str_currInsn.c_str(), apiName->Name, wantedApiAddr);
				memcpy(&encode[size - 4], &wantedApiAddr, 4);
				memcpy(currPointToBytecode, encode, size);
				memset(currPointToBytecode + size, '\x90', currInsn->size - size);
			}
			else if (regex_match(str_currInsn.c_str(), match, regex("(.+).dword.ptr.\\[0x(.+)\\]")))
			{
				DWORD callViaVA = -1;
				sscanf(string(match[2]).c_str(), "%x", &callViaVA);
				callViaVA -= getNtHdr(dumpImg)->OptionalHeader.ImageBase;

				map<DWORD, string>::iterator ret = callViaThunkArr.find(callViaVA);
				if (ret == callViaThunkArr.end()) continue;

				string impLibName = ret->second;
				char buf[64];
				PIMAGE_THUNK_DATA callVia = (PIMAGE_THUNK_DATA)(dumpImg + callViaVA);
				PIMAGE_IMPORT_BY_NAME apiName = (PIMAGE_IMPORT_BY_NAME)(dumpImg + callVia->u1.ForwarderString);
				size_t wantedApiAddr = (size_t)GetProcAddress(LoadLibraryA(impLibName.c_str()), apiName->Name);

				BYTE*encode; size_t count; size_t size;
				string newInsn = string(match[1]) + " 0x" + string(match[2]);

				asmblrOutputBytecode((char *)newInsn.c_str(), encode, size, count);
				printf("    # patch %p - %s -> %s@%x\n", currPointToBytecode, str_currInsn.c_str(), apiName->Name, wantedApiAddr);

				size_t jmpOrCall_Offset = (size_t)wantedApiAddr - (size_t)currPointToBytecode - size;
				memcpy(&encode[size - 4], &jmpOrCall_Offset, 4);
				memcpy(currPointToBytecode, encode, size);
				memset(currPointToBytecode + size, '\x90', currInsn->size - size);

			}
		}
	}
}

int main(void)
{
	puts
	(
		"dP                                   \n"
		"88                                   \n"
		"88        .d8888b. dP.  .dP .d8888b. \n"
		"88        88ooood8  `8bd8'  88'  `88 \n"
		"88        88.  ...  .d88b.  88.  .88 \n"
		"88888888P `88888P' dP'  `dP `88888P8 \n"
		"ooooooooooooooooooooooooooooooooooooo\n"
		"Lexa [v1.0] by aaaddress1@chroot.org\n"
		" --- \n"
	);
	// --- read target raw binary ---
	char* fileData; DWORD fileSize;
	readBinFile("msgbox.exe", &fileData, fileSize);

	// --- dump as mapped image ---
	BYTE* dumpImg; size_t imgSize;
	puts("[1]: File Mapping");
	dumpMappedImgBin(fileData, dumpImg, &imgSize);

	// --- parse iat fields ---
	puts("[2]: Fetch Imported Fields");
	PIMAGE_NT_HEADERS ntHdr = getNtHdr(dumpImg);
	map<DWORD, string> callViaThunkArr = getIAT_callVia((size_t)dumpImg);
	for (map<DWORD, string>::iterator iterator = callViaThunkArr.begin(); iterator != callViaThunkArr.end(); iterator++) {
		IMAGE_THUNK_DATA* fieldThunk = (IMAGE_THUNK_DATA*)(dumpImg + iterator->first);
		PIMAGE_IMPORT_BY_NAME currApiName = (PIMAGE_IMPORT_BY_NAME)(dumpImg + fieldThunk->u1.ForwarderString);
		printf("    # +%4x -> %s : %s\n", iterator->first, iterator->second.c_str(), &currApiName->Name);
	}
	// --- do relocation ---
	puts("[3]: Deal with Relocation");
	applyReloc((size_t)dumpImg, ntHdr->OptionalHeader.ImageBase, ntHdr->OptionalHeader.SizeOfImage);
	
	// ---
	/*puts("[4]: Fetch Imported Fields");
	deque<size_t> relocFieldArr = getRelocFieldArr((size_t)dumpImg, ntHdr->OptionalHeader.SizeOfImage);
	for (deque<size_t>::iterator iterator = relocFieldArr.begin(); iterator != relocFieldArr.end(); iterator++)
		printf("[+] reloc field -> %p\n", *iterator );
	*/

	// --- 
	puts("[4]: Patching for Importing IAT Fields");
	ntHdr->OptionalHeader.ImageBase = (size_t)dumpImg;
	patchIatImport(dumpImg, callViaThunkArr);

	// --- invoke entry ---
	printf("[5]: file mapping @ %p, entry = %p\n", dumpImg, dumpImg + ntHdr->OptionalHeader.AddressOfEntryPoint);
	((void(*)())(dumpImg + ntHdr->OptionalHeader.AddressOfEntryPoint))();
	
	return 0;
}