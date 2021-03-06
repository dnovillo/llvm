//===--------------------- Dispatch.cpp -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements methods declared by class RegisterFile and
/// DispatchUnit.
///
//===----------------------------------------------------------------------===//

#include "Dispatch.h"
#include "Backend.h"
#include "HWEventListener.h"
#include "Scheduler.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "llvm-mca"

namespace mca {

void RegisterFile::initialize(const MCSchedModel &SM, unsigned NumRegs) {
  // Create a default register file that "sees" all the machine registers
  // declared by the target. The number of physical registers in the default
  // register file is set equal to `NumRegs`. A value of zero for `NumRegs`
  // means: this register file has an unbounded number of physical registers.
  addRegisterFile({} /* all registers */, NumRegs);
  if (!SM.hasExtraProcessorInfo())
    return;

  // For each user defined register file, allocate a RegisterMappingTracker
  // object. The size of every register file, as well as the mapping between
  // register files and register classes is specified via tablegen.
  const MCExtraProcessorInfo &Info = SM.getExtraProcessorInfo();
  for (unsigned I = 0, E = Info.NumRegisterFiles; I < E; ++I) {
    const MCRegisterFileDesc &RF = Info.RegisterFiles[I];
    // Skip invalid register files with zero physical registers.
    unsigned Length = RF.NumRegisterCostEntries;
    if (!RF.NumPhysRegs)
      continue;
    // The cost of a register definition is equivalent to the number of
    // physical registers that are allocated at register renaming stage.
    const MCRegisterCostEntry *FirstElt =
        &Info.RegisterCostTable[RF.RegisterCostEntryIdx];
    addRegisterFile(ArrayRef<MCRegisterCostEntry>(FirstElt, Length),
                    RF.NumPhysRegs);
  }
}

void RegisterFile::addRegisterFile(ArrayRef<MCRegisterCostEntry> Entries,
                                   unsigned NumPhysRegs) {
  // A default register file is always allocated at index #0. That register file
  // is mainly used to count the total number of mappings created by all
  // register files at runtime. Users can limit the number of available physical
  // registers in register file #0 through the command line flag
  // `-register-file-size`.
  unsigned RegisterFileIndex = RegisterFiles.size();
  RegisterFiles.emplace_back(NumPhysRegs);

  // Special case where there is no register class identifier in the set.
  // An empty set of register classes means: this register file contains all
  // the physical registers specified by the target.
  if (Entries.empty()) {
    for (std::pair<WriteState *, IndexPlusCostPairTy> &Mapping :
         RegisterMappings)
      Mapping.second = std::make_pair(RegisterFileIndex, 1U);
    return;
  }

  // Now update the cost of individual registers.
  for (const MCRegisterCostEntry &RCE : Entries) {
    const MCRegisterClass &RC = MRI.getRegClass(RCE.RegisterClassID);
    for (const MCPhysReg Reg : RC) {
      IndexPlusCostPairTy &Entry = RegisterMappings[Reg].second;
      if (Entry.first) {
        // The only register file that is allowed to overlap is the default
        // register file at index #0. The analysis is inaccurate if register
        // files overlap.
        errs() << "warning: register " << MRI.getName(Reg)
               << " defined in multiple register files.";
      }
      Entry.first = RegisterFileIndex;
      Entry.second = RCE.Cost;
    }
  }
}

void RegisterFile::createNewMappings(IndexPlusCostPairTy Entry,
                                     MutableArrayRef<unsigned> UsedPhysRegs) {
  unsigned RegisterFileIndex = Entry.first;
  unsigned Cost = Entry.second;
  if (RegisterFileIndex) {
    RegisterMappingTracker &RMT = RegisterFiles[RegisterFileIndex];
    RMT.NumUsedMappings += Cost;
    UsedPhysRegs[RegisterFileIndex] += Cost;
  }

  // Now update the default register mapping tracker.
  RegisterFiles[0].NumUsedMappings += Cost;
  UsedPhysRegs[0] += Cost;
}

void RegisterFile::removeMappings(IndexPlusCostPairTy Entry,
                                  MutableArrayRef<unsigned> FreedPhysRegs) {
  unsigned RegisterFileIndex = Entry.first;
  unsigned Cost = Entry.second;
  if (RegisterFileIndex) {
    RegisterMappingTracker &RMT = RegisterFiles[RegisterFileIndex];
    RMT.NumUsedMappings -= Cost;
    FreedPhysRegs[RegisterFileIndex] += Cost;
  }

  // Now update the default register mapping tracker.
  RegisterFiles[0].NumUsedMappings -= Cost;
  FreedPhysRegs[0] += Cost;
}

void RegisterFile::addRegisterMapping(WriteState &WS,
                                      MutableArrayRef<unsigned> UsedPhysRegs) {
  unsigned RegID = WS.getRegisterID();
  assert(RegID && "Adding an invalid register definition?");

  RegisterMapping &Mapping = RegisterMappings[RegID];
  Mapping.first = &WS;
  for (MCSubRegIterator I(RegID, &MRI); I.isValid(); ++I)
    RegisterMappings[*I].first = &WS;

  createNewMappings(Mapping.second, UsedPhysRegs);

  // If this is a partial update, then we are done.
  if (!WS.fullyUpdatesSuperRegs())
    return;

  for (MCSuperRegIterator I(RegID, &MRI); I.isValid(); ++I)
    RegisterMappings[*I].first = &WS;
}

void RegisterFile::invalidateRegisterMapping(
    const WriteState &WS, MutableArrayRef<unsigned> FreedPhysRegs) {
  unsigned RegID = WS.getRegisterID();
  bool ShouldInvalidateSuperRegs = WS.fullyUpdatesSuperRegs();

  assert(RegID != 0 && "Invalidating an already invalid register?");
  assert(WS.getCyclesLeft() != -512 &&
         "Invalidating a write of unknown cycles!");
  assert(WS.getCyclesLeft() <= 0 && "Invalid cycles left for this write!");
  RegisterMapping &Mapping = RegisterMappings[RegID];
  if (!Mapping.first)
    return;

  removeMappings(Mapping.second, FreedPhysRegs);

  if (Mapping.first == &WS)
    Mapping.first = nullptr;

  for (MCSubRegIterator I(RegID, &MRI); I.isValid(); ++I)
    if (RegisterMappings[*I].first == &WS)
      RegisterMappings[*I].first = nullptr;

  if (!ShouldInvalidateSuperRegs)
    return;

  for (MCSuperRegIterator I(RegID, &MRI); I.isValid(); ++I)
    if (RegisterMappings[*I].first == &WS)
      RegisterMappings[*I].first = nullptr;
}

void RegisterFile::collectWrites(SmallVectorImpl<WriteState *> &Writes,
                                 unsigned RegID) const {
  assert(RegID && RegID < RegisterMappings.size());
  WriteState *WS = RegisterMappings[RegID].first;
  if (WS) {
    DEBUG(dbgs() << "Found a dependent use of RegID=" << RegID << '\n');
    Writes.push_back(WS);
  }

  // Handle potential partial register updates.
  for (MCSubRegIterator I(RegID, &MRI); I.isValid(); ++I) {
    WS = RegisterMappings[*I].first;
    if (WS && std::find(Writes.begin(), Writes.end(), WS) == Writes.end()) {
      DEBUG(dbgs() << "Found a dependent use of subReg " << *I << " (part of "
                   << RegID << ")\n");
      Writes.push_back(WS);
    }
  }
}

unsigned RegisterFile::isAvailable(ArrayRef<unsigned> Regs) const {
  SmallVector<unsigned, 4> NumPhysRegs(getNumRegisterFiles());

  // Find how many new mappings must be created for each register file.
  for (const unsigned RegID : Regs) {
    const IndexPlusCostPairTy &Entry = RegisterMappings[RegID].second;
    if (Entry.first)
      NumPhysRegs[Entry.first] += Entry.second;
    NumPhysRegs[0] += Entry.second;
  }

  unsigned Response = 0;
  for (unsigned I = 0, E = getNumRegisterFiles(); I < E; ++I) {
    unsigned NumRegs = NumPhysRegs[I];
    if (!NumRegs)
      continue;

    const RegisterMappingTracker &RMT = RegisterFiles[I];
    if (!RMT.TotalMappings) {
      // The register file has an unbounded number of microarchitectural
      // registers.
      continue;
    }

    if (RMT.TotalMappings < NumRegs) {
      // The current register file is too small. This may occur if the number of
      // microarchitectural registers in register file #0 was changed by the
      // users via flag -reg-file-size. Alternatively, the scheduling model
      // specified a too small number of registers for this register file.
      report_fatal_error(
          "Not enough microarchitectural registers in the register file");
    }

    if (RMT.TotalMappings < (RMT.NumUsedMappings + NumRegs))
      Response |= (1U << I);
  }

  return Response;
}

#ifndef NDEBUG
void RegisterFile::dump() const {
  for (unsigned I = 0, E = MRI.getNumRegs(); I < E; ++I) {
    const RegisterMapping &RM = RegisterMappings[I];
    dbgs() << MRI.getName(I) << ", " << I << ", Map=" << RM.second.first
           << ", ";
    if (RM.first)
      RM.first->dump();
    else
      dbgs() << "(null)\n";
  }

  for (unsigned I = 0, E = getNumRegisterFiles(); I < E; ++I) {
    dbgs() << "Register File #" << I;
    const RegisterMappingTracker &RMT = RegisterFiles[I];
    dbgs() << "\n  TotalMappings:        " << RMT.TotalMappings
           << "\n  NumUsedMappings:      " << RMT.NumUsedMappings << '\n';
  }
}
#endif

void DispatchUnit::notifyInstructionDispatched(const InstRef &IR,
                                               ArrayRef<unsigned> UsedRegs) {
  DEBUG(dbgs() << "[E] Instruction Dispatched: " << IR << '\n');
  Owner->notifyInstructionEvent(HWInstructionDispatchedEvent(IR, UsedRegs));
}

void DispatchUnit::notifyInstructionRetired(const InstRef &IR) {
  DEBUG(dbgs() << "[E] Instruction Retired: " << IR << '\n');
  SmallVector<unsigned, 4> FreedRegs(RAT->getNumRegisterFiles());
  for (const std::unique_ptr<WriteState> &WS : IR.getInstruction()->getDefs())
    RAT->invalidateRegisterMapping(*WS.get(), FreedRegs);
  Owner->notifyInstructionEvent(HWInstructionRetiredEvent(IR, FreedRegs));
  Owner->eraseInstruction(IR);
}

bool DispatchUnit::checkRAT(const InstRef &IR) {
  SmallVector<unsigned, 4> RegDefs;
  for (const std::unique_ptr<WriteState> &RegDef :
       IR.getInstruction()->getDefs())
    RegDefs.emplace_back(RegDef->getRegisterID());

  unsigned RegisterMask = RAT->isAvailable(RegDefs);
  // A mask with all zeroes means: register files are available.
  if (RegisterMask) {
    Owner->notifyStallEvent(HWStallEvent(HWStallEvent::RegisterFileStall, IR));
    return false;
  }

  return true;
}

bool DispatchUnit::checkRCU(const InstRef &IR) {
  const unsigned NumMicroOps = IR.getInstruction()->getDesc().NumMicroOps;
  if (RCU->isAvailable(NumMicroOps))
    return true;
  Owner->notifyStallEvent(
      HWStallEvent(HWStallEvent::RetireControlUnitStall, IR));
  return false;
}

bool DispatchUnit::checkScheduler(const InstRef &IR) {
  return SC->canBeDispatched(IR);
}

void DispatchUnit::updateRAWDependencies(ReadState &RS,
                                         const MCSubtargetInfo &STI) {
  SmallVector<WriteState *, 4> DependentWrites;

  collectWrites(DependentWrites, RS.getRegisterID());
  RS.setDependentWrites(DependentWrites.size());
  DEBUG(dbgs() << "Found " << DependentWrites.size() << " dependent writes\n");
  // We know that this read depends on all the writes in DependentWrites.
  // For each write, check if we have ReadAdvance information, and use it
  // to figure out in how many cycles this read becomes available.
  const ReadDescriptor &RD = RS.getDescriptor();
  if (!RD.HasReadAdvanceEntries) {
    for (WriteState *WS : DependentWrites)
      WS->addUser(&RS, /* ReadAdvance */ 0);
    return;
  }

  const MCSchedModel &SM = STI.getSchedModel();
  const MCSchedClassDesc *SC = SM.getSchedClassDesc(RD.SchedClassID);
  for (WriteState *WS : DependentWrites) {
    unsigned WriteResID = WS->getWriteResourceID();
    int ReadAdvance = STI.getReadAdvanceCycles(SC, RD.UseIndex, WriteResID);
    WS->addUser(&RS, ReadAdvance);
  }
  // Prepare the set for another round.
  DependentWrites.clear();
}

void DispatchUnit::dispatch(InstRef IR, const MCSubtargetInfo &STI) {
  assert(!CarryOver && "Cannot dispatch another instruction!");
  Instruction &IS = *IR.getInstruction();
  const InstrDesc &Desc = IS.getDesc();
  const unsigned NumMicroOps = Desc.NumMicroOps;
  if (NumMicroOps > DispatchWidth) {
    assert(AvailableEntries == DispatchWidth);
    AvailableEntries = 0;
    CarryOver = NumMicroOps - DispatchWidth;
  } else {
    assert(AvailableEntries >= NumMicroOps);
    AvailableEntries -= NumMicroOps;
  }

  // Update RAW dependencies if this instruction is not a zero-latency
  // instruction. The assumption is that a zero-latency instruction doesn't
  // require to be issued to the scheduler for execution. More importantly, it
  // doesn't have to wait on the register input operands.
  if (Desc.MaxLatency || !Desc.Resources.empty())
    for (std::unique_ptr<ReadState> &RS : IS.getUses())
      updateRAWDependencies(*RS, STI);

  // Allocate new mappings.
  SmallVector<unsigned, 4> RegisterFiles(RAT->getNumRegisterFiles());
  for (std::unique_ptr<WriteState> &WS : IS.getDefs())
    RAT->addRegisterMapping(*WS, RegisterFiles);

  // Reserve slots in the RCU, and notify the instruction that it has been
  // dispatched to the schedulers for execution.
  IS.dispatch(RCU->reserveSlot(IR, NumMicroOps));

  // Notify listeners of the "instruction dispatched" event.
  notifyInstructionDispatched(IR, RegisterFiles);

  // Now move the instruction into the scheduler's queue.
  // The scheduler is responsible for checking if this is a zero-latency
  // instruction that doesn't consume pipeline/scheduler resources.
  SC->scheduleInstruction(IR);
}

#ifndef NDEBUG
void DispatchUnit::dump() const {
  RAT->dump();
  RCU->dump();
}
#endif
} // namespace mca
