// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_UFS_CONTROLLER_PROVIDER_H_
#define T70_UFS_CONTROLLER_PROVIDER_H_
#include <IOKit/IOService.h>
#include "T70UfsReadOnly.h"

struct T70UfsControllerState;

class T70UfsControllerProvider : public IOService {
  OSDeclareDefaultStructors(T70UfsControllerProvider)
  T70UfsControllerState *state = nullptr;
protected:
  bool handleOpen(IOService *, IOOptionBits, void *) override;
  void handleClose(IOService *, IOOptionBits) override;
  bool handleIsOpen(const IOService *) const override;
public:
  IOService *probe(IOService *, SInt32 *) override;
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  bool isReadyForReadOnlyLu2() const;
  uint64_t lastLogicalBlock() const;
  uint32_t blockSize() const;
  T70_UFS_READ_STATUS readBlocks(uint64_t, uint64_t, void *, size_t);
  bool quiesceDmaForStop();
};
#endif
