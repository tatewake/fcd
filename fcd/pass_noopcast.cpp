//
// pass_noopcast.cpp
// Copyright (C) 2015 Félix Cloutier.
// All Rights Reserved.
//
// This file is distributed under the University of Illinois Open Source
// license. See LICENSE.md for details.
//

#include "passes.h"

#include <llvm/IR/PatternMatch.h>

using namespace llvm;
using namespace std;

// This pass targets this pattern:
//
// %foo.stack = type { i64, i32, i32* }
// %field0 = getelementptr inbounds %foo.stack, %foo.stack* %0, i64 0, i32 1
// %field1 = getelementptr inbounds %foo.stack, %foo.stack* %0, i64 0, i32 2
// %x = ptrtoint i32* %field0 to i64
// %y = bitcast i32** %field1 to i64*
// store i64 %x, i64* %y, align 8
//
// In this case, it's possible to eliminate both casts and do a straight
// store i32* %field0, i32** %field1, align 8
// This helps SSA formation as ptrtoint kills SROA.
// It won't be necessary after LLVM unifies pointer types into a single `*` type.

namespace
{
	struct NoopCastEliminator : public FunctionPass
	{
		static char ID;
		
		NoopCastEliminator() : FunctionPass(ID)
		{
		}
		
		static Value* uncastedValue(Value* value)
		{
			while (auto cast = dyn_cast<CastInst>(value))
			{
				if (cast->getOpcode() == CastInst::Trunc)
				{
					break;
				}
				value = cast->getOperand(0);
			}
			return value;
		}
		
		static GetElementPtrInst* gepUpToType(Value* pointer, Type* type)
		{
			assert(type->isPointerTy());
			PointerType* pointerType = cast<PointerType>(pointer->getType());
			Type* elementType = pointerType->getElementType();
			
			auto zero = ConstantInt::getNullValue(Type::getInt32Ty(pointer->getContext()));
			SmallVector<Value*, 4> gepIndices = {zero};
			while (Type* gepType = GetElementPtrInst::getIndexedType(elementType, gepIndices))
			{
				if (gepType->getPointerTo() == type)
				{
					return GetElementPtrInst::Create(nullptr, pointer, gepIndices);
				}
				gepIndices.push_back(zero);
			}
			return nullptr;
		}
		
		virtual bool runOnFunction(Function& fn) override
		{
			bool changed = false;
			for (BasicBlock& bb : fn)
			{
				auto iter = bb.begin();
				while (iter != bb.end())
				{
					if (auto store = dyn_cast<StoreInst>(iter))
					{
						Value* pointer = store->getPointerOperand();
						Value* storeValue = store->getValueOperand();
						Value* uncastedPointer = uncastedValue(pointer);
						Value* uncastedStoreValue = uncastedValue(storeValue);
						if (pointer != uncastedPointer && storeValue != uncastedStoreValue)
						if (auto pointerType = dyn_cast<PointerType>(uncastedPointer->getType()))
						{
							if (uncastedStoreValue->getType()->getPointerTo() != pointerType)
							if (auto subPointer = dyn_cast<PointerType>(pointerType->getElementType()))
							if (auto subValue = gepUpToType(uncastedStoreValue, subPointer))
							{
								subValue->insertBefore(store);
								uncastedStoreValue = subValue;
							}
							
							if (uncastedStoreValue->getType()->getPointerTo() == pointerType)
							{
								StoreInst* result = new StoreInst(uncastedStoreValue, uncastedPointer, store);
								store->eraseFromParent();
								iter = result->getIterator();
								changed = true;
							}
						}
					}
					++iter;
				}
			}
			return changed;
		}
	};
	
	char NoopCastEliminator::ID = 0;
	
	RegisterPass<NoopCastEliminator> noopCastElim("eliminatecasts", "Eliminate cast roundtrips");
}

