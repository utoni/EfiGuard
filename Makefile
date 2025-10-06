INSTALL=install
DESTDIR=/tmp
EDK2DIR=..
EDK2SETUPSCRIPT=./edksetup.sh
LDRNAME=Loader.efi
DXENAME=EfiGuardDxe.efi
FASTBOOT=0
DO_NOT_DISABLE_PATCHGUARD=0
EAC_COMPAT_MODE=1

EDK2FLAGS=-D EFIGUARD_DRIVER_FILENAME='$(DXENAME)'

ifeq ("$(wildcard $(EDK2DIR)/$(EDK2SETUPSCRIPT))","")
$(warning Setup script '$(EDK2DIR)/$(EDK2SETUPSCRIPT)' does not exist.)
endif

ifeq ("$(FASTBOOT)","1")
EDK2FLAGS+=-D FASTBOOT=1
endif
ifeq ("$(DO_NOT_DISABLE_PATCHGUARD)","1")
EDK2FLAGS+=-D DO_NOT_DISABLE_PATCHGUARD=1
endif
ifeq ("$(EAC_COMPAT_MODE)","1")
EDK2FLAGS+=-D EAC_COMPAT_MODE=1
endif

all: app efi

app:
	$(MAKE) -C Application/EfiDSEFix -f Makefile.mingw all

efi:
	cd '$(EDK2DIR)' && \
		. $(EDK2SETUPSCRIPT) && \
			build -a X64 -t GCC5 -p EfiGuardPkg/EfiGuardPkg.dsc -b RELEASE \
				$(EDK2FLAGS)

install: install-app install-efi

install-app: app
	$(INSTALL) -m0644 Application/EfiDSEFix/EfiDSEFix.exe '$(DESTDIR)/'

install-efi: efi
	cd '$(EDK2DIR)' && \
		$(INSTALL) -m0644 Build/EfiGuard/RELEASE_GCC5/X64/Loader.efi \
		'$(DESTDIR)/$(LDRNAME)'
	cd '$(EDK2DIR)' && \
		$(INSTALL) -m0644 Build/EfiGuard/RELEASE_GCC5/X64/EfiGuardDxe.efi \
		'$(DESTDIR)/$(DXENAME)'

help:
	@echo 'Targets: all app efi install install-app install-efi help'
	@echo
	@echo 'INSTALL                   = $(INSTALL)'
	@echo 'DESTDIR                   = $(DESTDIR)'
	@echo 'EDK2DIR                   = $(EDK2DIR)'
	@echo 'EDK2SETUPSCRIPT           = $(EDK2SETUPSCRIPT)'
	@echo 'LDRNAME                   = $(LDRNAME)'
	@echo 'DXENAME                   = $(DXENAME)'
	@echo 'FASTBOOT                  = $(FASTBOOT)'
	@echo 'DO_NOT_DISABLE_PATCHGUARD = $(DO_NOT_DISABLE_PATCHGUARD)'
	@echo 'EAC_COMPAT_MODE           = $(EAC_COMPAT_MODE)'
	@echo 'EFIGUARD_DRIVER_FILENAME  = $(EFIGUARD_DRIVER_FILENAME)'
	@echo 'EDK2FLAGS                 = $(EDK2FLAGS)'

.PHONY: all app efi install install-app install-efi help
