/*
	SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
	http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef NDEBUG
# define NDEBUG
#endif
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "PyrKernel.h"
#include "PyrObject.h"
#include "PyrPrimitive.h"
#include "PyrPrimitiveProto.h"
#include "PyrSignal.h"
#include "PyrSched.h"
#include "PyrSignalPrim.h"
#include "PyrFilePrim.h"
#include "PyrMathPrim.h"
#include "PyrListPrim.h"
#include "Opcodes.h"
#include "SC_InlineUnaryOp.h"
#include "SC_InlineBinaryOp.h"
#include "PyrMessage.h"
#include "PyrParseNode.h"
#include "PyrLexer.h"
#include "PyrKernelProto.h"
#include "PyrInterpreter.h"
#include "PyrObjectProto.h"
#include "PyrArchiverT.h"
#include "PyrDeepCopier.h"
#include "PyrDeepFreezer.h"
//#include "Wacom.h"
#include "InitAlloc.h"

#define SANITYCHECK 0
int yyparse();

extern bool gTraceInterpreter;
PyrSymbol *s_recvmsg;

void initPatternPrimitives();

typedef struct {
	PrimitiveHandler func;
	PyrSymbol* name;
	unsigned short base;
	unsigned char numArgs; 
	unsigned char varArgs;
	unsigned char keyArgs;
} PrimitiveDef;

typedef struct {
	int size, maxsize;
	PrimitiveDef *table;
} PrimitiveTable;

extern PrimitiveTable gPrimitiveTable;

extern PyrSlot o_nullframe;


int getPrimitiveNumArgs(int index)
{
	return gPrimitiveTable.table[index].numArgs;
}

PyrSymbol* getPrimitiveName(int index)
{
	return gPrimitiveTable.table[index].name;
}

int slotStrLen(PyrSlot *slot) {
        if (IsSym(slot)) {
                return slot->us->length;
        } else if (isKindOfSlot(slot, class_string)) {
                return slot->uo->size;
        }
        return -1;
}

int slotStrVal(PyrSlot *slot, char *str, int maxlen)
{
	if (IsSym(slot)) {
		strncpy(str, slot->us->name, maxlen);
		return errNone;
	} else if (isKindOfSlot(slot, class_string)) {
		int len;
		len = sc_min(maxlen-1, slot->uo->size);
		memcpy(str, slot->uos->s, len);
		str[len] = 0;
		return errNone;
	}
	return errWrongType;
}

int slotPStrVal(PyrSlot *slot, unsigned char *str)
{
	if (IsSym(slot)) {
		strncpy((char*)str+1, slot->us->name, 255);
		str[0] = slot->us->length;
		return errNone;
	} else if (isKindOfSlot(slot, class_string)) {
		int len;
		len = sc_min(255, slot->uo->size);
		memcpy(str+1, slot->uos->s, len);
		str[0] = len;
		return errNone;
	}
	return errWrongType;
}

int instVarAt(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	int index;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (a->utag != tagObj) return errWrongType;

	PyrObject *obj = a->uo;

	if (IsInt(b)) {
		index = b->ui;
		if (index < 0 || index >= obj->size) return errIndexOutOfRange;
		a->ucopy = obj->slots[index].ucopy;
	} else if (IsSym(b)) {
		PyrSlot *instVarNamesSlot = &obj->classptr->instVarNames;
		if (!isKindOfSlot(instVarNamesSlot, class_symbolarray)) return errFailed;
		PyrSymbolArray *instVarNames = instVarNamesSlot->uosym;
		PyrSymbol **names = instVarNames->symbols;
		PyrSymbol *name = b->us;
		for (int i=0; i<instVarNames->size; ++i) {
			if (names[i] == name) {
				a->ucopy = obj->slots[i].ucopy;
				return errNone;
			}
		}
		return errFailed;
	} else return errWrongType;
	return errNone;
}

int instVarPut(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, *c, *slot;
	int index;
	PyrObject *obj;
	
	a = g->sp - 2;
	b = g->sp - 1;
	c = g->sp;
	
	if (a->utag != tagObj) return errWrongType;
	obj = a->uo;
	if (obj->obj_flags & obj_immutable) return errImmutableObject;

	if (IsInt(b)) {
		index = b->ui;
		if (index < 0 || index >= obj->size) return errIndexOutOfRange;
		slot = obj->slots + index;
		slot->ucopy = c->ucopy;
		g->gc->GCWrite(obj, slot);
	} else if (IsSym(b)) {
		PyrSlot *instVarNamesSlot = &obj->classptr->instVarNames;
		if (!IsObj(instVarNamesSlot)) return errFailed;
		PyrSymbolArray *instVarNames = instVarNamesSlot->uosym;
		PyrSymbol **names = instVarNames->symbols;
		PyrSymbol *name = b->us;
		for (int i=0; i<instVarNames->size; ++i) {
			if (names[i] == name) {
				slot = obj->slots + i;
				slot->ucopy = c->ucopy;
				g->gc->GCWrite(obj, slot);
				return errNone;
			}
		}
		post("WARNING: %s instVarPut '%s' failed.\n", obj->classptr->name.us->name, name->name);
		return errNone;
	} else return errWrongType;
	return errNone;
}

int instVarSize(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrObject *obj;
	
	a = g->sp;
	if (a->utag != tagObj) {
		SetInt(a, 0);
		return errNone;
	}
	obj = a->uo;
	if (obj->obj_format == obj_notindexed) {
		SetInt(a, obj->size);
	} else {
		SetInt(a, 0);
	}
	return errNone;
}


int objectHash(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	int hash;

	a = g->sp;
	
	hash = calcHash(a);
	SetInt(a, hash);
	return errNone;
}

int objectClass(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrClass *classobj;
	
	a = g->sp;
	classobj = classOfSlot(a);
	SetObject(a, classobj);
	return errNone;
}

int prPrimitiveError(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	a->ucopy = g->thread->primitiveError.ucopy;
	return errNone;
}

int prStackDepth(struct VMGlobals *g, int numArgsPushed);
int prStackDepth(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	SetInt(a, g->gc->StackDepth());
	return errNone;
}

extern void DumpStack(VMGlobals *g, PyrSlot *sp);

int prDumpStack(struct VMGlobals *g, int numArgsPushed)
{
	DumpStack(g, g->sp);
	return errNone;
}

void DumpDetailedBackTrace(VMGlobals *g);
int prDumpDetailedBackTrace(struct VMGlobals *g, int numArgsPushed);
int prDumpDetailedBackTrace(struct VMGlobals *g, int numArgsPushed)
{
	DumpDetailedBackTrace(g);
	return errNone;
}

int prPrimitiveErrorString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrString *string;
	char *str;
	
	a = g->sp;
	switch (g->thread->primitiveError.ui) {
		case errReturn : str = "Return (not an error)."; break;
		case errNone : str = "No Error"; break;
		case errFailed : str = "Failed."; break;
		case errBadPrimitive : str = "Bad Primitive."; break;
		case errWrongType : str = "Wrong type."; break;
		case errIndexNotAnInteger : str = "Index not an Integer"; break;
		case errIndexOutOfRange : str = "Index out of range."; break;
		case errImmutableObject : str = "Attempted write to immutable object."; break;
		case errNotAnIndexableObject : str = "Not an indexable object."; break;
		case errStackOverflow : str = "Stack overflow."; break;
		case errOutOfMemory : str = "Out of memory."; break;
		case errCantCallOS : str = "operation cannot be called from this Process."; break;
		
		default : str = "Failed.";
	}
	string = newPyrString(g->gc, str, 0, true);
	SetObject(a, string);
	return errNone;
}



int prPostString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	//if (a->utag != tagObj) return errWrongType;
	// assume it is a string!
	postText(a->uos->s, a->uos->size);
	return errNone;
}

int prPostLine(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	//if (a->utag != tagObj) return errWrongType;
	// assume it is a string!
	postText(a->uos->s, a->uos->size);
	postChar('\n');
	return errNone;
}

int prDebugger(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	//Debugger();
	return errNone;
}




int prObjectString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrString *string;
	char str[256];
	
	a = g->sp;
	if (postString(a, str)) {
		string = newPyrString(g->gc, str, 0, true);
		SetObject(a, string);
		return errNone;
	} else {
		return errFailed;
	}
}

int prFloat_AsStringPrec(struct VMGlobals *g, int numArgsPushed);
int prFloat_AsStringPrec(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp - 1;
	PyrSlot *b = g->sp;
	
	int precision;
	int err = slotIntVal(b, &precision);
	if (err) return err;
	
	char fmt[8], str[32];
	sprintf(fmt, "%%.%dg", precision);
	sprintf(str, fmt, a->uf);
	
	PyrString *string = newPyrString(g->gc, str, 0, true);
	SetObject(a, string);
	return errNone;
	
}

int prAsCompileString(struct VMGlobals *g, int numArgsPushed);
int prAsCompileString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrString *string;
	char str[256];
	int err;
	
	a = g->sp;
	err = asCompileString(a, str);
	if (err == errNone) {
		string = newPyrString(g->gc, str, 0, true);
		SetObject(a, string);
	}
	return err;
}


int prClassString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrClass *classobj;
	PyrString *string;
	
	a = g->sp;
	classobj = classOfSlot(a);
	string = newPyrString(g->gc, classobj->name.us->name, 0, true);
	SetObject(a, string);
	return errNone;
}


int prPrimName(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrThread *thread;
	
	a = g->sp;
	thread = a->uot;
	if (thread->primitiveIndex.ui <= gPrimitiveTable.size) {
		SetSymbol(a, gPrimitiveTable.table[thread->primitiveIndex.ui].name);
	} else {
		SetSymbol(a, s_none);
	}
	return errNone;
}

int objectIsKindOf(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	PyrClass *classobj, *testclass;
	int objClassIndex, testClassIndex, maxSubclassIndex;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (b->utag != tagObj) return errWrongType;
	testclass = (PyrClass*)b->uo;
	classobj = classOfSlot(a);
#if 0
	while (classobj) {
		if (classobj == testclass) {
			SetTrue(a);
			return errNone;
		}
		classobj = classobj->superclass.us->u.classobj;
	}
	SetFalse(a);
#else 	
	// constant time lookup method:
	
	objClassIndex = classobj->classIndex.ui;
	testClassIndex = testclass->classIndex.ui;
	maxSubclassIndex = testclass->maxSubclassIndex.ui;

	/*post("%s %s\n", classobj->name.us->name, testclass->name.us->name);
	post("objClassIndex %d\n", objClassIndex);
	post("testClassIndex %d\n", testClassIndex);
	post("maxSubclassIndex %d\n", maxSubclassIndex);*/

	if (objClassIndex >= testClassIndex && objClassIndex <= maxSubclassIndex) {
		SetTrue(a);
		return errNone;
	} else {
		SetFalse(a);
		return errNone;
	}

#endif
	return errNone;
}


int objectIsMemberOf(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	PyrClass *classobj, *testclass;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (b->utag != tagObj) return errWrongType;
	testclass = (PyrClass*)b->uo;
	classobj = classOfSlot(a);
	if (classobj == testclass) {
		SetTrue(a);
	} else {
		SetFalse(a);
	}
	return errNone;
}

int objectIdentical(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (a->ui == b->ui && a->utag == b->utag) {
		SetTrue(a);
	} else {
		SetFalse(a);
	}
	return errNone;
}

int objectNotIdentical(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (a->ui != b->ui || a->utag != b->utag) {
		SetTrue(a);
	} else {
		SetFalse(a);
	}
	return errNone;
}


int basicNewClear(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	int size;
	PyrClass *classobj;
	PyrObject *newobj;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (a->utag != tagObj) return errWrongType;
	classobj = (PyrClass*)a->uo;
	if (classobj->classFlags.ui & classHasIndexableInstances) {
		// create an indexable object
		if (b->utag != tagInt) {
			if (IsFloat(b)) {
				size = (int)b->uf;
			} else if (NotNil(b)) return errIndexNotAnInteger;
			else size = 8;
		} else {
			size = b->ui;
		}
		if (size < 0) size = 0;
	} else {
		size = 0;
	}
	newobj = instantiateObject(g->gc, classobj, size, true, true);
	SetObject(a, newobj);
	return errNone;
}	

int basicNewCopyArgsToInstanceVars(struct VMGlobals *g, int numArgsPushed);
int basicNewCopyArgsToInstanceVars(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	PyrClass *classobj;
	PyrObject *newobj;
	
	a = g->sp - numArgsPushed + 1;
	b = a + 1;
	
	if (a->utag != tagObj) return errWrongType;
	classobj = (PyrClass*)a->uo;
	if (classobj->classFlags.ui & classHasIndexableInstances) {
		error("CopyArgs : object has no instance variables.\n");
		return errFailed;
	}
	newobj = instantiateObject(g->gc, classobj, 0, true, true);
	SetObject(a, newobj);
	
	int length = sc_min(numArgsPushed-1, newobj->size);
	for (int i=0; i<length; ++i) {
		newobj->slots[i].ucopy = b[i].ucopy;
	}
	
	return errNone;
}	



int basicNew(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	int size;
	PyrClass *classobj;
	PyrObject *newobj;
		
	a = g->sp - 1;
	b = g->sp;
	
	if (a->utag != tagObj) return errWrongType;
	classobj = (PyrClass*)a->uo;
	if (classobj->classFlags.ui & classHasIndexableInstances) {
		// create an indexable object
		if (b->utag != tagInt) {
			if (IsFloat(b)) {
				size = (int)b->uf;
			} else if (NotNil(b)) return errIndexNotAnInteger;
			else size = 8;
		} else {
			size = b->ui;
		}
		if (size < 0) size = 0;
	} else {
		size = 0;
	}
	newobj = instantiateObject(g->gc, classobj, size, false, true);
	SetObject(a, newobj);
	return errNone;
}	


bool isClosed(PyrBlock* fundef);
bool isClosed(PyrBlock* fundef)
{
	return IsNil(&fundef->contextDef) && fundef->classptr == class_fundef;
}

bool isWithinClosed(PyrBlock* fundef);
bool isWithinClosed(PyrBlock* fundef)
{
	while (fundef) {
		if (isClosed(fundef)) return true;
		fundef = fundef->contextDef.uoblk;
	}
	return false;
}

int prFunctionDefAsFunction(struct VMGlobals *g, int numArgsPushed);
int prFunctionDefAsFunction(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;

	if (!isClosed(a->uoblk)) {
		dumpObjectSlot(a);
		error("Only closed FunctionDef may be converted to a Function using asFunction.\n");
		return errFailed;
	}

	PyrClosure* closure = (PyrClosure*)g->gc->New(2*sizeof(PyrSlot), 0, obj_notindexed, true);

	closure->classptr = gSpecialClasses[op_class_func]->u.classobj;
	closure->size = 2;
	closure->block.ucopy = a->ucopy;
	closure->context.ucopy = g->process->interpreter.uoi->context.ucopy;
	SetObject(a, closure);
	return errNone;
}

int prFunctionDefDumpContexts(struct VMGlobals *g, int numArgsPushed);
int prFunctionDefDumpContexts(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;

	int i=0;
	while (a->uoblk) {
		post("%2d context %s %08X\n", i++, a->uo->classptr->name.us->name, a->uoblk->contextDef.ui);
		a = &a->uoblk->contextDef;
	}
	return errNone;
}



int prFunctionDefIsClosed(struct VMGlobals *g, int numArgsPushed);
int prFunctionDefIsClosed(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	PyrBlock *block = a->uoblk;
	
	SetBool(a, isClosed(block));
	return errNone;
}

int prFunctionDefIsWithinClosed(struct VMGlobals *g, int numArgsPushed);
int prFunctionDefIsWithinClosed(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	PyrBlock *block = a->uoblk;
	
	SetBool(a, isWithinClosed(block));
	return errNone;
}


void reallocStack(struct VMGlobals *g, int stackNeeded, int stackDepth)
{
	PyrThread *thread = g->thread;
	PyrGC *gc = g->gc;
	int newStackSize = NEXTPOWEROFTWO(stackNeeded);
	SetInt(&thread->stackSize, newStackSize);
	
	PyrObject* array = newPyrArray(gc, newStackSize, 0, false);	
	memcpy(array->slots, gc->Stack()->slots, stackDepth * sizeof(PyrSlot));
	gc->SetStack(array);
	gc->ToBlack(gc->Stack());
	g->sp = array->slots + stackDepth - 1;
}


int blockValueArray(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *b;
	PyrObject *array;
	PyrList *list;
	double *pslot, *qslot;
	int m, size;
	
	//a = g->sp - numArgsPushed + 1;
	b = g->sp;
	
	if (b->utag == tagObj) {
		if (b->uo->classptr == class_array) {
			array = (PyrObject*)b->uo;
			above:
			size = array->size;

			PyrObject *stack = g->gc->Stack();
			int stackDepth = g->sp - stack->slots + 1;
			int stackSize = ARRAYMAXINDEXSIZE(stack);
			int stackNeeded = stackDepth + array->size + 64;  // 64 to allow extra for normal stack operations.
			if (stackNeeded > stackSize) {
				reallocStack(g, stackNeeded, stackDepth);
				b = g->sp;
			}
			
			pslot = (double*)(array->slots - 1);
			qslot = (double*)(b - 1);
			//pend = (double*)(pslot + size);
			//while (pslot<pend) *++qslot = *++pslot;
			for (m=0; m<size; ++m) *++qslot = *++pslot;

			g->sp += size - 1;
			return blockValue(g, size+numArgsPushed-1);
			
		} else if (b->uo->classptr == class_list) {
			list = b->uol;
			if (list->array.utag != tagObj) return errWrongType;
			array = list->array.uo; 
			if (array->classptr != class_array) return errWrongType;
			goto above;
		} else { // last arg is not a list or array, so pass as normal
			return blockValue(g, numArgsPushed);
		}
	} else {
		return blockValue(g, numArgsPushed);
	}
}

int blockValueEnvir(struct VMGlobals *g, int numArgsPushed);

int blockValueArrayEnvir(struct VMGlobals *g, int numArgsPushed);
int blockValueArrayEnvir(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *b;
	PyrObject *array;
	PyrList *list;
	double *pslot, *qslot;
	int m, size;
	
	//a = g->sp - numArgsPushed + 1;
	b = g->sp;
	
	if (b->utag == tagObj) {
		if (b->uo->classptr == class_array) {
			array = (PyrObject*)b->uo;
			above:
			size = array->size;

			PyrObject *stack = g->gc->Stack();
			int stackDepth = g->sp - stack->slots + 1;
			int stackSize = ARRAYMAXINDEXSIZE(stack);
			int stackNeeded = stackDepth + array->size + 64;  // 64 to allow extra for normal stack operations.
			if (stackNeeded > stackSize) {
				reallocStack(g, stackNeeded, stackDepth);
				b = g->sp;
			}
			
			pslot = (double*)(array->slots - 1);
			qslot = (double*)(b - 1);
			//pend = (double*)(pslot + size);
			//while (pslot<pend) *++qslot = *++pslot;
			for (m=0; m<size; ++m) *++qslot = *++pslot;

			g->sp += size - 1;
			return blockValueEnvir(g, size+numArgsPushed-1);
			
		} else if (b->uo->classptr == class_list) {
			list = b->uol;
			if (list->array.utag != tagObj) return errWrongType;
			array = list->array.uo; 
			if (array->classptr != class_array) return errWrongType;
			goto above;
		} else { // last arg is not a list or array, so pass as normal
			return blockValueEnvir(g, numArgsPushed);
		}
	} else {
		return blockValueEnvir(g, numArgsPushed);
	}
}

int blockValue(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *args;
	PyrSlot *vars;
	PyrFrame *frame;
	double *pslot, *qslot;
	PyrSlot *rslot;
	PyrObject *proto;
	int i, m, mmax, numtemps;
	PyrBlock *block;
	PyrFrame *context;
	PyrFrame *caller;
	PyrFrame *homeContext;
	PyrClosure *closure;
	PyrMethodRaw *methraw;
	
	int tailCall = g->tailCall;
	if (tailCall) {
		/*if (g->method) {
			postfl("tailCall %d   %s-%s -> %s-%s\n", 
				tailCall,
				g->method->ownerclass.uoc->name.us->name, g->method->name.us->name,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		} else {
			postfl("tailCall %d    top -> %s-%s\n", 
				tailCall,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		}*/
		if (tailCall == 1) {
			returnFromMethod(g);
		} else {
			returnFromBlock(g);
		}
	}

	g->execMethod = 30;

	args = g->sp - numArgsPushed + 1;
	
	numArgsPushed -- ;
	g->numpop = 0;
	
	closure = (PyrClosure*)args->uo;
	block = closure->block.uoblk;
	context = closure->context.uof;
	
	proto = block->prototypeFrame.uo;
	methraw = METHRAW(block);
	numtemps = methraw->numtemps;
	caller = g->frame;

	frame = (PyrFrame*)g->gc->NewFrame(methraw->frameSize, 0, obj_slot, methraw->needsHeapContext);
	vars = frame->vars - 1;
	frame->classptr = class_frame;
	frame->size = FRAMESIZE + numtemps;
	SetObject(&frame->method, block);
	frame->homeContext.ucopy = context->homeContext.ucopy;
	frame->context.ucopy = closure->context.ucopy;
	
	if (caller) {
		SetInt(&caller->ip, (int)g->ip);
		SetObject(&frame->caller, g->frame);
	} else {
		SetNil(&frame->caller);
	}
	SetInt(&frame->ip,  0);


	g->sp = args - 1;
	g->ip = block->code.uob->b - 1;
	g->frame = frame;
	g->block = block;
	
	if (numArgsPushed <= methraw->numargs) {	/* not enough args pushed */
		/* push all args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);

		for (m=0; m<numArgsPushed; ++m) *++pslot = *++qslot;
		
		/* push default arg values */
		pslot = (double*)(vars + numArgsPushed);
		qslot = (double*)(proto->slots + numArgsPushed - 1);
		for (m=0; m<numtemps - numArgsPushed; ++m) *++pslot = *++qslot;
	} else if (methraw->varargs) {
		PyrObject *list;
		double *lslot;
		
		/* push all normal args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);
		for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		
		/* push list */
		i = numArgsPushed - methraw->numargs;
		list = newPyrArray(g->gc, i, 0, false);
		list->size = i;
		
		rslot = (PyrSlot*)pslot+1;
		SetObject(rslot, list);
		//SetObject(vars + methraw->numargs + 1, list);
		
		/* put extra args into list */
		lslot = (double*)(list->slots - 1);
		// fixed and raw sizes are zero
		for (m=0; m<i; ++m) *++lslot = *++qslot;
		
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs + 1);
			qslot = (double*)(proto->slots + methraw->numargs);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	} else {
		if (methraw->numargs) {
			/* push all args to frame */
			qslot = (double*)(args);
			pslot = (double*)(vars);
			for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		}
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs);
			qslot = (double*)(proto->slots + methraw->numargs - 1);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	}

	homeContext = frame->homeContext.uof;
	if (homeContext) {
		PyrMethodRaw *methraw;
		g->method = homeContext->method.uom;
		methraw = METHRAW(g->method);
		g->receiver.ucopy = homeContext->vars[0].ucopy;
	} else {
		g->receiver.ucopy = g->process->interpreter.ucopy;
	}
	
	return errNone;
}

int blockValueWithKeys(VMGlobals *g, int allArgsPushed, int numKeyArgsPushed);
int blockValueWithKeys(VMGlobals *g, int allArgsPushed, int numKeyArgsPushed)
{
	PyrSlot *args;
	PyrSlot *vars;
	PyrFrame *frame;
	double *pslot, *qslot;
	PyrSlot *rslot;
	PyrObject *proto;
	int i, j, m, mmax, numtemps, numArgsPushed;
	PyrBlock *block;
	PyrFrame *context;
	PyrFrame *caller;
	PyrFrame *homeContext;
	PyrClosure *closure;
	PyrMethodRaw *methraw;
	
	int tailCall = g->tailCall;
	if (tailCall) {
		/*if (g->method) {
			postfl("tailCall %d   %s-%s -> %s-%s\n", 
				tailCall,
				g->method->ownerclass.uoc->name.us->name, g->method->name.us->name,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		} else {
			postfl("tailCall %d    top -> %s-%s\n", 
				tailCall,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		}*/
		if (tailCall == 1) {
			returnFromMethod(g);
		} else {
			returnFromBlock(g);
		}
	}
	
	g->execMethod = 40;

	args = g->sp - allArgsPushed + 1;

	allArgsPushed -- ;
	g->numpop = 0;
	
	closure = (PyrClosure*)args->uo;
	block = closure->block.uoblk;
	context = closure->context.uof;
	
	proto = block->prototypeFrame.uo;
	methraw = METHRAW(block);
	numtemps = methraw->numtemps;
	caller = g->frame;
	numArgsPushed = allArgsPushed - (numKeyArgsPushed<<1);

	frame = (PyrFrame*)g->gc->NewFrame(methraw->frameSize, 0, obj_slot, methraw->needsHeapContext);
	vars = frame->vars - 1;
	frame->classptr = class_frame;
	frame->size = FRAMESIZE + numtemps;
	SetObject(&frame->method, block);
	frame->homeContext.ucopy = context->homeContext.ucopy;
	frame->context.ucopy = closure->context.ucopy;
	
	if (caller) {
		SetInt(&caller->ip, (int)g->ip);
		SetObject(&frame->caller, g->frame);
	} else {
		SetNil(&frame->caller);
	}
	SetInt(&frame->ip,  0);

	g->sp = args - 1;
	g->ip = block->code.uob->b - 1;
	g->frame = frame;
	g->block = block;
	
	if (numArgsPushed <= methraw->numargs) {	/* not enough args pushed */
		/* push all args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);

		for (m=0; m<numArgsPushed; ++m) *++pslot = *++qslot;
		
		/* push default arg values */
		pslot = (double*)(vars + numArgsPushed);
		qslot = (double*)(proto->slots + numArgsPushed - 1);
		for (m=0; m<numtemps - numArgsPushed; ++m) *++pslot = *++qslot;
	} else if (methraw->varargs) {
		PyrObject *list;
		double *lslot;
		
		/* push all normal args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);
		for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		
		/* push list */
		i = numArgsPushed - methraw->numargs;
		list = newPyrArray(g->gc, i, 0, false);
		list->size = i;
		
		rslot = (PyrSlot*)pslot+1;
		SetObject(rslot, list);
		//SetObject(vars + methraw->numargs + 1, list);
		
		/* put extra args into list */
		lslot = (double*)(list->slots - 1);
		// fixed and raw sizes are zero
		//lend = lslot + i;
		//while (lslot < lend) *++lslot = *++qslot;
		for (m=0; m<i; ++m) *++lslot = *++qslot;
		
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs + 1);
			qslot = (double*)(proto->slots + methraw->numargs);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	} else {
		if (methraw->numargs) {
			/* push all args to frame */
			qslot = (double*)(args);
			pslot = (double*)(vars);
			//pend = pslot + methraw->numargs;
			//while (pslot < pend) *++pslot = *++qslot;
			for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		}
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs);
			qslot = (double*)(proto->slots + methraw->numargs - 1);
			//pend = pslot + methraw->numvars;
			//while (pslot<pend) *++pslot = *++qslot;
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	}
	// do keyword lookup:
	if (numKeyArgsPushed && methraw->posargs) {
		PyrSlot *key;
		PyrSymbol **name0, **name;
		name0 = block->argNames.uosym->symbols;
		key = args + numArgsPushed + 1;
		for (i=0; i<numKeyArgsPushed; ++i, key+=2) {
			name = name0;
			for (j=0; j<methraw->posargs; ++j, ++name) {
				if (*name == key->us) {
					vars[j+1].ucopy = key[1].ucopy;
					goto found1;
				}
			}
			if (gKeywordError) {
				post("WARNING: keyword arg '%s' not found in call to function.\n",
					key->us->name);
			}
			found1: ;
		}
	}

	homeContext = frame->homeContext.uof;
	if (homeContext) {
		PyrMethodRaw *methraw;
		g->method = homeContext->method.uom;
		methraw = METHRAW(g->method);
		g->receiver.ucopy = homeContext->vars[0].ucopy;
	} else {
		g->receiver.ucopy = g->process->interpreter.ucopy;
	}
	return errNone;
}

bool identDict_lookupNonNil(PyrObject *dict, PyrSlot *key, int hash, PyrSlot *result);

int blockValueEnvir(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *args;
	PyrSlot *vars;
	PyrFrame *frame;
	double *pslot, *qslot;
	PyrSlot *rslot;
	PyrObject *proto;
	int i, m, mmax, numtemps;
	PyrBlock *block;
	PyrFrame *context;
	PyrFrame *caller;
	PyrFrame *homeContext;
	PyrClosure *closure;
	PyrMethodRaw *methraw;
	PyrSlot *curEnvirSlot;
	
	int tailCall = g->tailCall;
	if (tailCall) {
		/*if (g->method) {
			postfl("tailCall %d   %s-%s -> %s-%s\n", 
				tailCall,
				g->method->ownerclass.uoc->name.us->name, g->method->name.us->name,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		} else {
			postfl("tailCall %d    top -> %s-%s\n", 
				tailCall,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		}*/
		if (tailCall == 1) {
			returnFromMethod(g);
		} else {
			returnFromBlock(g);
		}
	}
		
	g->execMethod = 50;

	args = g->sp - numArgsPushed + 1;

	numArgsPushed -- ;
	g->numpop = 0;
	
	closure = (PyrClosure*)args->uo;
	block = closure->block.uoblk;
	context = closure->context.uof;
	
	proto = block->prototypeFrame.uo;
	methraw = METHRAW(block);
	numtemps = methraw->numtemps;
	caller = g->frame;

	frame = (PyrFrame*)g->gc->NewFrame(methraw->frameSize, 0, obj_slot, methraw->needsHeapContext);
	vars = frame->vars - 1;
	frame->classptr = class_frame;
	frame->size = FRAMESIZE + numtemps;
	SetObject(&frame->method, block);
	frame->homeContext.ucopy = context->homeContext.ucopy;
	frame->context.ucopy = closure->context.ucopy;
	
	if (caller) {
		SetInt(&caller->ip, (int)g->ip);
		SetObject(&frame->caller, g->frame);
	} else {
		SetNil(&frame->caller);
	}
	SetInt(&frame->ip,  0);


	g->sp = args - 1;
	g->ip = block->code.uob->b - 1;
	g->frame = frame;
	g->block = block;
	
	if (numArgsPushed <= methraw->numargs) {	/* not enough args pushed */
		/* push all args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);

		for (m=0; m<numArgsPushed; ++m) *++pslot = *++qslot;
		
		/* push default arg values */
		pslot = (double*)(vars + numArgsPushed);
		qslot = (double*)(proto->slots + numArgsPushed - 1);
		for (m=0; m<numtemps - numArgsPushed; ++m) *++pslot = *++qslot;

		// replace defaults with environment variables
		curEnvirSlot = g->classvars[0].uo->slots + 1; // currentEnvironment is the second class var.

		if (isKindOfSlot(curEnvirSlot, s_identitydictionary->u.classobj)) {
			PyrSymbol **argNames;
			argNames = block->argNames.uosym->symbols;
			for (m=numArgsPushed; m<methraw->numargs; ++m) {
				// replace the args with values from the environment if they exist
				PyrSlot keyslot;
				SetSymbol(&keyslot, argNames[m]);
				identDict_lookupNonNil(curEnvirSlot->uo, &keyslot, calcHash(&keyslot), vars+m+1);
			}
		}
	} else if (methraw->varargs) {
		PyrObject *list;
		double *lslot;
		
		/* push all normal args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);
		for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		
		/* push list */
		i = numArgsPushed - methraw->numargs;
		list = newPyrArray(g->gc, i, 0, false);
		list->size = i;
		
		rslot = (PyrSlot*)pslot+1;
		SetObject(rslot, list);
		//SetObject(vars + methraw->numargs + 1, list);
		
		/* put extra args into list */
		lslot = (double*)(list->slots - 1);
		// fixed and raw sizes are zero
		for (m=0; m<i; ++m) *++lslot = *++qslot;
		
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs + 1);
			qslot = (double*)(proto->slots + methraw->numargs);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	} else {
		if (methraw->numargs) {
			/* push all args to frame */
			qslot = (double*)(args);
			pslot = (double*)(vars);
			for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		}
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs);
			qslot = (double*)(proto->slots + methraw->numargs - 1);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	}

	homeContext = frame->homeContext.uof;
	if (homeContext) {
		PyrMethodRaw *methraw;
		g->method = homeContext->method.uom;
		methraw = METHRAW(g->method);
		g->receiver.ucopy = homeContext->vars[0].ucopy;
	} else {
		g->receiver.ucopy = g->process->interpreter.ucopy;
	}
	return errNone;
}

int blockValueEnvirWithKeys(VMGlobals *g, int allArgsPushed, int numKeyArgsPushed);
int blockValueEnvirWithKeys(VMGlobals *g, int allArgsPushed, int numKeyArgsPushed)
{
	PyrSlot *args;
	PyrSlot *vars;
	PyrFrame *frame;
	double *pslot, *qslot;
	PyrSlot *rslot;
	PyrObject *proto;
	int i, j, m, mmax, numtemps, numArgsPushed;
	PyrBlock *block;
	PyrFrame *context;
	PyrFrame *caller;
	PyrFrame *homeContext;
	PyrClosure *closure;
	PyrMethodRaw *methraw;
	PyrSlot *curEnvirSlot;
	
	int tailCall = g->tailCall;
	if (tailCall) {
		/*if (g->method) {
			postfl("tailCall %d   %s-%s -> %s-%s\n", 
				tailCall,
				g->method->ownerclass.uoc->name.us->name, g->method->name.us->name,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		} else {
			postfl("tailCall %d    top -> %s-%s\n", 
				tailCall,
				meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		}*/
		if (tailCall == 1) {
			returnFromMethod(g);
		} else {
			returnFromBlock(g);
		}
	}
	
	g->execMethod = 60;

	args = g->sp - allArgsPushed + 1;

	allArgsPushed -- ;
	g->numpop = 0;
	
	closure = (PyrClosure*)args->uo;
	block = closure->block.uoblk;
	context = closure->context.uof;
	
	proto = block->prototypeFrame.uo;
	methraw = METHRAW(block);
	numtemps = methraw->numtemps;
	caller = g->frame;
	numArgsPushed = allArgsPushed - (numKeyArgsPushed<<1);

	frame = (PyrFrame*)g->gc->NewFrame(methraw->frameSize, 0, obj_slot, methraw->needsHeapContext);
	vars = frame->vars - 1;
	frame->classptr = class_frame;
	frame->size = FRAMESIZE + numtemps;
	SetObject(&frame->method, block);
	frame->homeContext.ucopy = context->homeContext.ucopy;
	frame->context.ucopy = closure->context.ucopy;
	
	if (caller) {
		SetInt(&caller->ip, (int)g->ip);
		SetObject(&frame->caller, g->frame);
	} else {
		SetNil(&frame->caller);
	}
	SetInt(&frame->ip,  0);


	g->sp = args - 1;
	g->ip = block->code.uob->b - 1;
	g->frame = frame;
	g->block = block;
	
	if (numArgsPushed <= methraw->numargs) {	/* not enough args pushed */
		/* push all args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);

		for (m=0; m<numArgsPushed; ++m) *++pslot = *++qslot;
		
		/* push default arg values */
		pslot = (double*)(vars + numArgsPushed);
		qslot = (double*)(proto->slots + numArgsPushed - 1);
		for (m=0; m<numtemps - numArgsPushed; ++m) *++pslot = *++qslot;
		
		// replace defaults with environment variables
		curEnvirSlot = g->classvars[0].uo->slots + 1; // currentEnvironment is the second class var.

		if (isKindOfSlot(curEnvirSlot, s_identitydictionary->u.classobj)) {
			PyrSymbol **argNames;
			argNames = block->argNames.uosym->symbols;
			for (m=numArgsPushed; m<methraw->numargs; ++m) {
				// replace the args with values from the environment if they exist
				PyrSlot keyslot;
				SetSymbol(&keyslot, argNames[m]);
				identDict_lookupNonNil(curEnvirSlot->uo, &keyslot, calcHash(&keyslot), vars+m+1);
			}
		}
		
		
	} else if (methraw->varargs) {
		PyrObject *list;
		double *lslot;
		
		/* push all normal args to frame */
		qslot = (double*)(args);
		pslot = (double*)(vars);
		for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		
		/* push list */
		i = numArgsPushed - methraw->numargs;
		list = newPyrArray(g->gc, i, 0, false);
		list->size = i;
		
		rslot = (PyrSlot*)pslot+1;
		SetObject(rslot, list);
		//SetObject(vars + methraw->numargs + 1, list);
		
		/* put extra args into list */
		lslot = (double*)(list->slots - 1);
		// fixed and raw sizes are zero
		//lend = lslot + i;
		//while (lslot < lend) *++lslot = *++qslot;
		for (m=0; m<i; ++m) *++lslot = *++qslot;
		
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs + 1);
			qslot = (double*)(proto->slots + methraw->numargs);
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	} else {
		if (methraw->numargs) {
			/* push all args to frame */
			qslot = (double*)(args);
			pslot = (double*)(vars);
			//pend = pslot + methraw->numargs;
			//while (pslot < pend) *++pslot = *++qslot;
			for (m=0,mmax=methraw->numargs; m<mmax; ++m) *++pslot = *++qslot;
		}
		if (methraw->numvars) {
			/* push default keyword and var values */
			pslot = (double*)(vars + methraw->numargs);
			qslot = (double*)(proto->slots + methraw->numargs - 1);
			//pend = pslot + methraw->numvars;
			//while (pslot<pend) *++pslot = *++qslot;
			for (m=0,mmax=methraw->numvars; m<mmax; ++m) *++pslot = *++qslot;
		}
	}
	// do keyword lookup:
	if (numKeyArgsPushed && methraw->posargs) {
		PyrSymbol **name0, **name;
		PyrSlot *key;
		name0 = block->argNames.uosym->symbols;
		key = args + numArgsPushed + 1;
		for (i=0; i<numKeyArgsPushed; ++i, key+=2) {
			name = name0;
			for (j=0; j<methraw->posargs; ++j, ++name) {
				if (*name == key->us) {
					vars[j+1].ucopy = key[1].ucopy;
					goto found1;
				}
			}
			if (gKeywordError) {
				post("WARNING: keyword arg '%s' not found in call to function.\n",
					key->us->name);
			}
			found1: ;
		}
	}
	
	homeContext = frame->homeContext.uof;
	if (homeContext) {
		PyrMethodRaw *methraw;
		g->method = homeContext->method.uom;
		methraw = METHRAW(g->method);
		g->receiver.ucopy = homeContext->vars[0].ucopy;
	} else {
		g->receiver.ucopy = g->process->interpreter.ucopy;
	}
	return errNone;
}


int objectPerform(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax;
	
	recvrSlot = g->sp - numArgsPushed + 1;
	selSlot = recvrSlot + 1;
	if (IsSym(selSlot)) {
		selector = selSlot->us;
		// move args down one to fill selector's position
		pslot = (double*)(selSlot - 1);
		qslot = (double*)selSlot;
		for (m=0; m<numArgsPushed - 2; ++m) *++pslot = *++qslot;
		g->sp -- ;
		numArgsPushed -- ;
		// now the stack looks just like it would for a normal message send
	} else if (IsObj(selSlot)) {
		listSlot = selSlot;
		if (listSlot->uo->classptr == class_list) {
			listSlot = listSlot->uo->slots;
		}
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			goto badselector;
		}
		PyrObject *array = listSlot->uo;
		if (array->size < 1) {
			error("Array must have a selector.\n");
			return errFailed; 
		}
		selSlot = array->slots;
		selector = selSlot->us;

		if (numArgsPushed>2) {
			qslot = (double*)recvrSlot + numArgsPushed;
			pslot = (double*)recvrSlot + numArgsPushed + array->size - 2;
			for (m=0; m<numArgsPushed - 2; ++m) *--pslot = *--qslot;
		}
		
		pslot = (double*)(recvrSlot);
		qslot = (double*)(selSlot);
		for (m=0,mmax=array->size-1; m<mmax; ++m) *++pslot = *++qslot;
		
		g->sp += array->size - 2;
		numArgsPushed += array->size - 2;
		// now the stack looks just like it would for a normal message send
		
	} else {
		badselector:
		error("perform selector not a Symbol or Array.\n");
		dumpObjectSlot(selSlot);
		return errWrongType; 
	}
	
	sendMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	return errNone;
}

int objectPerformWithKeys(VMGlobals *g, int numArgsPushed, int numKeyArgsPushed);
int objectPerformWithKeys(VMGlobals *g, int numArgsPushed, int numKeyArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax;
	
	recvrSlot = g->sp - numArgsPushed + 1;
	selSlot = recvrSlot + 1;
	if (IsSym(selSlot)) {
		selector = selSlot->us;
		// move args down one to fill selector's position
		pslot = (double*)(selSlot - 1);
		qslot = (double*)selSlot;
		for (m=0; m<numArgsPushed - 2; ++m) *++pslot = *++qslot;
		g->sp -- ;
		numArgsPushed -- ;
		// now the stack looks just like it would for a normal message send
	} else if (IsObj(selSlot)) {
		listSlot = selSlot;
		if (listSlot->uo->classptr == class_list) {
			listSlot = listSlot->uo->slots;
		}
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			goto badselector;
		}
		PyrObject *array = listSlot->uo;
		if (array->size < 1) {
			error("Array must have a selector.\n");
			return errFailed; 
		}
		selSlot = array->slots;
		selector = selSlot->us;

		if (numArgsPushed>2) {
			qslot = (double*)recvrSlot + numArgsPushed;
			pslot = (double*)recvrSlot + numArgsPushed + array->size - 2;
			for (m=0; m<numArgsPushed - 2; ++m) *--pslot = *--qslot;
		}
		
		pslot = (double*)(recvrSlot);
		qslot = (double*)(selSlot);
		for (m=0,mmax=array->size-1; m<mmax; ++m) *++pslot = *++qslot;
		
		g->sp += array->size - 2;
		numArgsPushed += array->size - 2;
		// now the stack looks just like it would for a normal message send
		
	} else {
		badselector:
		error("perform selector not a Symbol or Array.\n");
		dumpObjectSlot(selSlot);
		return errWrongType; 
	}
	
	sendMessageWithKeys(g, selector, numArgsPushed, numKeyArgsPushed);
	g->numpop = 0;
	return errNone;
}


int objectPerformList(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax, numargslots;
	PyrObject *array;
	
	
	recvrSlot = g->sp - numArgsPushed + 1;
	selSlot = recvrSlot + 1;
	listSlot = g->sp;
	numargslots = numArgsPushed - 3;
	if (selSlot->utag != tagSym) {
		error("Selector not a Symbol :\n");
		return errWrongType;
	}
	selector = selSlot->us;
	
	if (listSlot->utag != tagObj) {
		return objectPerform(g, numArgsPushed);
	}
	if (listSlot->uo->classptr == class_array) {
		doarray:
		array = listSlot->uo;
		
		PyrObject *stack = g->gc->Stack();
		int stackDepth = g->sp - stack->slots + 1;
		int stackSize = ARRAYMAXINDEXSIZE(stack);
		int stackNeeded = stackDepth + array->size + 64;  // 64 to allow extra for normal stack operations.
		if (stackNeeded > stackSize) {
			reallocStack(g, stackNeeded, stackDepth);
			recvrSlot = g->sp - numArgsPushed + 1;
			selSlot = recvrSlot + 1;
		}
		
		pslot = (double*)(recvrSlot);
		if (numargslots>0) {
			qslot = (double*)selSlot;
			for (m=0; m<numargslots; ++m) *++pslot = *++qslot;
		} else numargslots = 0;
		qslot = (double*)(array->slots - 1);
		for (m=0,mmax=array->size; m<mmax; ++m) *++pslot = *++qslot;
	} else if (listSlot->uo->classptr == class_list) {
		listSlot = listSlot->uo->slots;
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			error("List array not an Array.\n");
			dumpObjectSlot(listSlot);
			return errWrongType; 
		}
		goto doarray;
	} else {
		return objectPerform(g, numArgsPushed);
	}
	g->sp += array->size - 2;
	numArgsPushed = numargslots + array->size + 1;
	// now the stack looks just like it would for a normal message send
	
	sendMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	
	return errNone;
}


int objectSuperPerform(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax;
	
	recvrSlot = g->sp - numArgsPushed + 1;

	PyrClass* classobj = g->method->ownerclass.uoc->superclass.us->u.classobj;
	if (!isKindOfSlot(recvrSlot, classobj)) {
		error("superPerform must be called with 'this' as the receiver.\n");
		return errFailed;
	}

	selSlot = recvrSlot + 1;
	if (IsSym(selSlot)) {
		selector = selSlot->us;
		// move args down one to fill selector's position
		pslot = (double*)(selSlot - 1);
		qslot = (double*)selSlot;
		for (m=0; m<numArgsPushed - 2; ++m) *++pslot = *++qslot;
		g->sp -- ;
		numArgsPushed -- ;
		// now the stack looks just like it would for a normal message send
	} else if (IsObj(selSlot)) {
		listSlot = selSlot;
		if (listSlot->uo->classptr == class_list) {
			listSlot = listSlot->uo->slots;
		}
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			goto badselector;
		}
		PyrObject *array = listSlot->uo;
		if (array->size < 1) {
			error("Array must have a selector.\n");
			return errFailed; 
		}
		selSlot = array->slots;
		selector = selSlot->us;

		if (numArgsPushed>2) {
			qslot = (double*)recvrSlot + numArgsPushed;
			pslot = (double*)recvrSlot + numArgsPushed + array->size - 2;
			for (m=0; m<numArgsPushed - 2; ++m) *--pslot = *--qslot;
		}
		
		pslot = (double*)(recvrSlot);
		qslot = (double*)(selSlot);
		for (m=0,mmax=array->size-1; m<mmax; ++m) *++pslot = *++qslot;
		
		g->sp += array->size - 2;
		numArgsPushed += array->size - 2;
		// now the stack looks just like it would for a normal message send
		
	} else {
		badselector:
		error("perform selector not a Symbol or Array.\n");
		dumpObjectSlot(selSlot);
		return errWrongType; 
	}
	
	sendSuperMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	return errNone;
}

int objectSuperPerformWithKeys(VMGlobals *g, int numArgsPushed, int numKeyArgsPushed);
int objectSuperPerformWithKeys(VMGlobals *g, int numArgsPushed, int numKeyArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax;
	
	recvrSlot = g->sp - numArgsPushed + 1;

	PyrClass* classobj = g->method->ownerclass.uoc->superclass.us->u.classobj;
	if (!isKindOfSlot(recvrSlot, classobj)) {
		error("superPerform must be called with 'this' as the receiver.\n");
		return errFailed;
	}

	selSlot = recvrSlot + 1;
	if (IsSym(selSlot)) {
		selector = selSlot->us;
		// move args down one to fill selector's position
		pslot = (double*)(selSlot - 1);
		qslot = (double*)selSlot;
		for (m=0; m<numArgsPushed - 2; ++m) *++pslot = *++qslot;
		g->sp -- ;
		numArgsPushed -- ;
		// now the stack looks just like it would for a normal message send
	} else if (IsObj(selSlot)) {
		listSlot = selSlot;
		if (listSlot->uo->classptr == class_list) {
			listSlot = listSlot->uo->slots;
		}
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			goto badselector;
		}
		PyrObject *array = listSlot->uo;
		if (array->size < 1) {
			error("Array must have a selector.\n");
			return errFailed; 
		}
		selSlot = array->slots;
		selector = selSlot->us;

		if (numArgsPushed>2) {
			qslot = (double*)recvrSlot + numArgsPushed;
			pslot = (double*)recvrSlot + numArgsPushed + array->size - 2;
			for (m=0; m<numArgsPushed - 2; ++m) *--pslot = *--qslot;
		}
		
		pslot = (double*)(recvrSlot);
		qslot = (double*)(selSlot);
		for (m=0,mmax=array->size-1; m<mmax; ++m) *++pslot = *++qslot;
		
		g->sp += array->size - 2;
		numArgsPushed += array->size - 2;
		// now the stack looks just like it would for a normal message send
		
	} else {
		badselector:
		error("perform selector not a Symbol or Array.\n");
		dumpObjectSlot(selSlot);
		return errWrongType; 
	}
	
	sendSuperMessageWithKeys(g, selector, numArgsPushed, numKeyArgsPushed);
	g->numpop = 0;
	return errNone;
}


int objectSuperPerformList(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax, numargslots;
	PyrObject *array;
	
	recvrSlot = g->sp - numArgsPushed + 1;
	selSlot = recvrSlot + 1;
	listSlot = g->sp;
	numargslots = numArgsPushed - 3;
	if (selSlot->utag != tagSym) {
		error("Selector not a Symbol :\n");
		return errWrongType;
	}
	selector = selSlot->us;
	if (listSlot->utag != tagObj) {
		return objectPerform(g, numArgsPushed);
	}
	if (listSlot->uo->classptr == class_array) {
		doarray:
		pslot = (double*)(recvrSlot);
		if (numargslots>0) {
			qslot = (double*)selSlot;
			for (m=0; m<numargslots; ++m) *++pslot = *++qslot;
		} else numargslots = 0;
		array = listSlot->uo;
		qslot = (double*)(array->slots - 1);
		for (m=0,mmax=array->size; m<mmax; ++m) *++pslot = *++qslot;
	} else if (listSlot->uo->classptr == class_list) {
		listSlot = listSlot->uo->slots;
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			error("List array not an Array.\n");
			dumpObjectSlot(listSlot);
			return errWrongType; 
		}
		goto doarray;
	} else {
		return objectSuperPerform(g, numArgsPushed);
	}
	g->sp += array->size - 2;
	numArgsPushed = numargslots + array->size + 1;
	// now the stack looks just like it would for a normal message send
	
	sendSuperMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	return errNone;
}




int objectPerformSelList(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *listSlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax;
	PyrObject *array;
	
	recvrSlot = g->sp - 1;
	listSlot = g->sp;

	if (listSlot->utag != tagObj) {
		error("Expected Array or List.. Got :\n");
		dumpObjectSlot(listSlot);
		return errWrongType;
	}
	if (listSlot->uo->classptr == class_array) {
		doarray:
		array = listSlot->uo;
		
		selSlot = array->slots;
		if (selSlot->utag != tagSym) {
			error("Selector not a Symbol :\n");
			return errWrongType;
		}
		selector = selSlot->us;
		
		pslot = (double*)(recvrSlot);
		qslot = (double*)(selSlot);
		for (m=0,mmax=array->size-1; m<mmax; ++m) *++pslot = *++qslot;
	} else if (listSlot->uo->classptr == class_list) {
		listSlot = listSlot->uo->slots;
		if (listSlot->utag != tagObj || listSlot->uo->classptr != class_array) {
			error("List array not an Array.\n");
			dumpObjectSlot(listSlot);
			return errWrongType; 
		}
		goto doarray;
	} else {
		error("Expected Array or List.. Got :\n");
		dumpObjectSlot(listSlot);
		return errWrongType;
	}
	g->sp += array->size - 2;
	numArgsPushed = array->size;
	// now the stack looks just like it would for a normal message send
	
	sendMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	return errNone;
}


int arrayPerformMsg(struct VMGlobals *g, int numArgsPushed);
int arrayPerformMsg(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *recvrSlot, *selSlot, *arraySlot;
	double *pslot, *qslot;
	PyrSymbol *selector;
	int m, mmax, numargslots;
	PyrObject *array;
	
	arraySlot = g->sp - numArgsPushed + 1;
	array = arraySlot->uo;
	if (array->size < 2) {
		error("Array must contain a receiver and a selector.\n");
		return errFailed;
	}
	recvrSlot = array->slots;
	selSlot = recvrSlot + 1;
	numargslots = numArgsPushed - 1;
	if (selSlot->utag != tagSym) {
		error("Selector not a Symbol :\n");
		return errWrongType;
	}
	
	selector = selSlot->us;

	arraySlot->ucopy = recvrSlot->ucopy;

	if (numargslots>0) {
		qslot = (double*)arraySlot + numargslots + 1;
		pslot = (double*)arraySlot + numargslots + array->size - 1;
		for (m=0; m<numargslots; ++m) *--pslot = *--qslot;
	} else numargslots = 0;
	
	pslot = (double*)(arraySlot);
	qslot = (double*)(selSlot);
	for (m=0,mmax=array->size-2; m<mmax; ++m) *++pslot = *++qslot;
		
	g->sp += array->size - 2;
	numArgsPushed = numargslots + array->size - 1;
	
	// now the stack looks just like it would for a normal message send
	
	sendMessage(g, selector, numArgsPushed);
	g->numpop = 0;
	return errNone;
}

int objectDump(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	dumpObjectSlot(a);
	return errNone;
}


int prTotalFree(struct VMGlobals *g, int numArgsPushed);
int prTotalFree(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	SetInt(a, g->allocPool->TotalFree());
	return errNone;
}

int prLargestFreeBlock(struct VMGlobals *g, int numArgsPushed);
int prLargestFreeBlock(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	SetInt(a, g->allocPool->LargestFreeChunk());
	return errNone;
}

int dumpGCinfo(struct VMGlobals *g, int numArgsPushed);
int dumpGCinfo(struct VMGlobals *g, int numArgsPushed)
{
	g->gc->DumpInfo();
	return errNone;
}

int dumpGCdumpGrey(struct VMGlobals *g, int numArgsPushed);
int dumpGCdumpGrey(struct VMGlobals *g, int numArgsPushed)
{
	g->gc->DumpGrey();
	return errNone;
}

int dumpGCdumpSet(struct VMGlobals *g, int numArgsPushed);
int dumpGCdumpSet(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *b = g->sp;
	int set;
	int err = slotIntVal(b, &set);
	if (err) return err;
	
	g->gc->DumpSet(set);
	return errNone;
}

int prGCSanity(struct VMGlobals *g, int numArgsPushed);
int prGCSanity(struct VMGlobals *g, int numArgsPushed)
{
	g->gc->SanityCheck();
	return errNone;
}

#if GCDEBUG
int prTraceAllPathsTo(struct VMGlobals *g, int numArgsPushed);
int prTraceAllPathsTo(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	g->gc->TracePathsTo(a->uo, false);
	return errNone;
}

int prTraceAnyPathsTo(struct VMGlobals *g, int numArgsPushed);
int prTraceAnyPathsTo(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	g->gc->TracePathsTo(a->uo, true);
	return errNone;
}

int prTraceAnyPathToAllInstancesOf(struct VMGlobals *g, int numArgsPushed);
int prTraceAnyPathToAllInstancesOf(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	g->gc->TraceAnyPathToAllInstancesOf(a->uoc->name.us);
	return errNone;
}
#endif

extern PyrClass *gClassList;

int prAllClasses(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrClass *classobj;
	PyrObject *array;
	int i;
	
	a = g->sp;
	
	array = newPyrArray(g->gc, gNumClasses, 0, true); 
	classobj = gClassList;
	for (i=0; classobj; ++i) {
		SetObject(array->slots + i, classobj);
		classobj = classobj->nextclass.uoc;
	}
	array->size = gNumClasses;
	SetObject(a, array);
	return errNone;
}

int prPostClassTree(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	postClassTree(a->uoc, 0);
	return errNone;
}

int prDumpBackTrace(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	DumpBackTrace(g);
	return errNone;
}


void MakeDebugFrame(VMGlobals *g, PyrFrame *frame, PyrSlot *outSlot);
void MakeDebugFrame(VMGlobals *g, PyrFrame *frame, PyrSlot *outSlot)
{
	int i, j;
	PyrMethod *meth;
	PyrMethodRaw *methraw;
		
	meth = frame->method.uom;
	methraw = METHRAW(meth);
	
	PyrObject* debugFrameObj = instantiateObject(g->gc, getsym("DebugFrame")->u.classobj, 0, false, true);
	SetObject(outSlot, debugFrameObj);
	
	SetObject(debugFrameObj->slots + 0, meth);
	SetInt(debugFrameObj->slots + 5, (int)meth);
	
	//int numtemps = methraw->numargs;
	int numargs = methraw->numargs;
	int numvars = methraw->numvars;
	if (numargs) {
		PyrObject* argArray = (PyrObject*)newPyrArray(g->gc, numargs, 0, false);
		SetObject(debugFrameObj->slots + 1, argArray);
		for (i=0; i<numargs; ++i) {
			argArray->slots[i].ucopy = frame->vars[i].ucopy;
		}
		argArray->size = numargs;
	} else {
		SetNil(debugFrameObj->slots + 1);
	}
	if (numvars) {
		PyrObject* varArray = (PyrObject*)newPyrArray(g->gc, numvars, 0, false);
		SetObject(debugFrameObj->slots + 2, varArray);
		for (i=0,j=numargs; i<numvars; ++i,++j) {
			varArray->slots[i].ucopy = frame->vars[j].ucopy;
		}
		varArray->size = numvars;
	} else {
		SetNil(debugFrameObj->slots + 2);
	}
	
	if (NotNil(&frame->caller)) {
		MakeDebugFrame(g, frame->caller.uof, debugFrameObj->slots + 3);
	} else {
		SetNil(debugFrameObj->slots + 3);
	}
	
	if (frame->context.utag == tagObj && frame->context.uof == frame) {
		SetObject(debugFrameObj->slots + 4,  debugFrameObj);
	} else if (NotNil(&frame->context)) {
		MakeDebugFrame(g, frame->context.uof, debugFrameObj->slots + 4);
	} else {
		SetNil(debugFrameObj->slots + 4);
	}
}


int prGetBackTrace(VMGlobals *g, int numArgsPushed);
int prGetBackTrace(VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	MakeDebugFrame(g, g->frame, a);
	
	return errNone;
}

int prObjectShallowCopy(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	switch (a->utag) {
		case tagObj :
			a->uo = copyObject(g->gc, a->uo, true);
			break;
		// the default case is to leave the argument unchanged on the stack			
	}
	return errNone;
}

int prObjectCopyImmutable(struct VMGlobals *g, int numArgsPushed);
int prObjectCopyImmutable(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	switch (a->utag) {
		case tagObj :
			if (a->uo->obj_flags & obj_immutable) {
				a->uo = copyObject(g->gc, a->uo, true);
			}
			break;
	}
	return errNone;
}

int prObjectIsMutable(struct VMGlobals *g, int numArgsPushed);
int prObjectIsMutable(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	if (a->utag == tagObj) {
		if (a->uo->obj_flags & obj_immutable) {
			SetFalse(a);
		} else {
			SetTrue(a);
		}
	} else {
		SetFalse(a);
	}
	return errNone;
}

int prObjectIsPermanent(struct VMGlobals *g, int numArgsPushed);
int prObjectIsPermanent(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	if (a->utag == tagObj) {
		if (a->uo->gc_color == obj_permanent) {
			SetTrue(a);
		} else {
			SetFalse(a);
		}
	} else {
		SetTrue(a);
	}
	return errNone;
}



int prDeepFreeze(struct VMGlobals *g, int numArgsPushed);
int prDeepFreeze(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	if (g->processID != 0) {
		error("Permanent objects may only be created from the non real time virtual machine.\n");
		return errFailed;
	}

	a = g->sp;
	PyrDeepFreezer freezer(g);
	int err = freezer.doDeepFreeze(a);
	return err;
}


int prDeepCopy(struct VMGlobals *g, int numArgsPushed);
int prDeepCopy(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	if (g->processID != 0) {
		error("Permanent objects may only be created from the non real time virtual machine.\n");
		return errFailed;
	}

	a = g->sp;	
	PyrDeepCopier copier(g);
	int err = copier.doDeepCopy(a);
	return err;
}




struct RemoteMsg { 
	int32 processID, numSlots;
	PyrSlot slots[32];
};

bool IsSimpleLiteralSlot(PyrSlot* slot);
bool IsSimpleLiteralSlot(PyrSlot* slot)
{
	switch (slot->utag) {
		case tagObj : return slot->uo->IsPermanent();
		case tagInt : return true;
		case tagSym : return true;
		case tagChar : return true;
		case tagNil : return true;
		case tagFalse : return true;
		case tagTrue : return true;
		case tagPtr : return false;
		default : return true;
	}
}



int prObjectCopyRange(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, *c;
	
	a = g->sp - 2;
	b = g->sp - 1;
	c = g->sp;
	
	if (a->utag != tagObj) return errWrongType;
	if (b->utag != tagInt) return errWrongType;
	if (c->utag != tagInt) return errWrongType;
	a->uo = copyObjectRange(g->gc, a->uo, b->ui, c->ui, true);

	return errNone;
}


int prObjectCopySeries(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, *c, *d;
	
	a = g->sp - 3;
	b = g->sp - 2;
	c = g->sp - 1;
	d = g->sp;
		
	PyrObject *inobj = a->uo;
	PyrObject *newobj;
	
	int size = inobj->size;
	int flags = ~(obj_immutable) & inobj->obj_flags;
	
	int first, second, last;
	
	if (IsInt(b)) first = b->ui;
	else if (IsNil(b)) first = 0;
	else return errWrongType;

	if (IsInt(d)) {
		last = d->ui;
		if (last < 0 && IsNil(b)) {
zerolength:
			newobj = g->gc->New(0, flags, inobj->obj_format, true);
			newobj->size = 0;
			newobj->classptr = inobj->classptr;
			a->uo = newobj;
			return errNone;
		}
	} else if (IsNil(d)) {
		if (first >= size) goto zerolength;
		last = size - 1;
	} else return errWrongType;

	if (IsInt(c)) second = c->ui;
	else if (IsNil(c)) second = first < last ? b->ui + 1 : b->ui - 1;
	else return errWrongType;

	int step = second - first;
	
	int elemsize = gFormatElemSize[inobj->obj_format];
	int length;
	
	if (step > 0) {
		length = (last - first) / step + 1;
	} else if (step < 0) {
		length = (first - last) / -step + 1;
	} else return errFailed;

	int numbytes = length * elemsize;
	
	newobj = g->gc->New(numbytes, flags, inobj->obj_format, true);
	newobj->size = 0;
	newobj->classptr = inobj->classptr;

	for (int i=first, j=0; j<length; i+=step, ++j) {
		PyrSlot slot;
		if (i >= 0 && i < inobj->size) {
			getIndexedSlot(inobj, &slot, i);
			int err = putIndexedSlot(g, newobj, &slot, newobj->size++);
			if (err) return err;
		}
	}

	a->uo = newobj;
	return errNone;
}

void switchToThread(struct VMGlobals *g, struct PyrThread *newthread, int oldstate, int *numArgsPushed);

int haltInterpreter(struct VMGlobals *g, int numArgsPushed)
{
	switchToThread(g, g->process->mainThread.uot, tDone, &numArgsPushed);
	g->returnLevels = LONG_MAX;
	SetNil(g->sp);
	//hmm need to fix this to work only on main thread. //!!!
	//g->sp = g->gc->Stack()->slots - 1;
	
	return errReturn;
}


int prCanCallOS(struct VMGlobals *g, int numArgsPushed);
int prCanCallOS(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a = g->sp;
	
	SetBool(a, g->canCallOS);
	
	return errNone;
}


int prTraceOn(struct VMGlobals *g, int numArgsPushed);
int prTraceOn(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	gTraceInterpreter = IsTrue(a);
	return errNone;
}

int prKeywordError(struct VMGlobals *g, int numArgsPushed);
int prKeywordError(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	gKeywordError = IsTrue(a);
	return errNone;
}

int prFunDef_NumArgs(struct VMGlobals *g, int numArgsPushed);
int prFunDef_NumArgs(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrMethodRaw *methraw;
	
	a = g->sp;
	methraw = METHRAW(a->uoblk);
	SetInt(a, methraw->numargs);
	return errNone;
}

int prFunDef_NumVars(struct VMGlobals *g, int numArgsPushed);
int prFunDef_NumVars(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrMethodRaw *methraw;
	
	a = g->sp;
	methraw = METHRAW(a->uoblk);
	SetInt(a, methraw->numvars);
	return errNone;
}

int prFunDef_VarArgs(struct VMGlobals *g, int numArgsPushed);
int prFunDef_VarArgs(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	PyrMethodRaw *methraw;
	
	a = g->sp;
	methraw = METHRAW(a->uoblk);
	if (methraw->varargs) { SetTrue(a); } else { SetFalse(a); }
	return errNone;
}


int undefinedPrimitive(struct VMGlobals *g, int numArgsPushed)
{
	error("A primitive was not bound. %d %d\n", g->primitiveIndex, gPrimitiveTable.size);
	dumpObject((PyrObject*)g->primitiveMethod);
	return errFailed;
}

void dumpByteCodes(PyrBlock *theBlock);

int prDumpByteCodes(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;
	
	a = g->sp;
	dumpByteCodes(a->uoblk);
	return errNone;
}

int prObjectPointsTo(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, temp;
	PyrObject *obj;
	int i;
	
	a = g->sp - 1;
	b = g->sp;
	
	if (a->utag != tagObj) a->ucopy = o_false.ucopy;
	else {
		obj = a->uo;
		for (i=0; i<obj->size; ++i) {
			getIndexedSlot(obj, &temp, i);
			if (temp.utag == b->utag && temp.ui == b->ui) {
				a->ucopy = o_true.ucopy;
				return errNone;
			}
		}
		a->ucopy = o_false.ucopy;
	}
	return errNone;
}


int prObjectRespondsTo(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	PyrClass *classobj;
	PyrMethod *meth;
	PyrSymbol *selector;
	int index;
	
	a = g->sp - 1;
	b = g->sp;

	classobj = classOfSlot(a);
	
	if (b->utag == tagSym) {
			
		selector = b->us;
		index = classobj->classIndex.ui + selector->u.index;
		meth = gRowTable[index];
		if (meth->name.us != selector) {
			a->ucopy = o_false.ucopy;
		} else {
			a->ucopy = o_true.ucopy;
		}
	} else if (isKindOfSlot(b, class_array)) {
		int size = b->uo->size;
		PyrSlot *slot = b->uo->slots;
		for (int i=0; i<size; ++i, ++slot) {
			
			if (slot->utag != tagSym) return errWrongType;
			
			selector = slot->us;
			index = classobj->classIndex.ui + selector->u.index;
			meth = gRowTable[index];
			if (meth->name.us != selector) {
				a->ucopy = o_false.ucopy;
				return errNone;
			}
		}
		a->ucopy = o_true.ucopy;
	} else return errWrongType;
	return errNone;
}

PyrMethod* GetFunctionCompileContext(VMGlobals* g);
PyrMethod* GetFunctionCompileContext(VMGlobals* g)
{
	PyrClass *classobj;
	PyrSymbol *classsym, *contextsym;
	PyrMethod *meth;
	// lookup interpreter class
	classsym = getsym("Interpreter");
	classobj = classsym->u.classobj;
	if (!classobj) {
		error("There is no Interpreter class.\n");
		return 0;
	}
	// lookup functionCompileContext method
	contextsym = getsym("functionCompileContext");
	int index = classobj->classIndex.ui + contextsym->u.index;
	meth = gRowTable[index];
	if (!meth || meth->name.us != contextsym) {
		error("compile context method 'functionCompileContext' not found.\n");
		return 0;
	}
	gCompilingClass = classobj;
	gCompilingMethod = meth;
	gCompilingBlock = (PyrBlock*)meth;
	return meth;
}

#if !SCPLAYER
int prCompileString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b;
	PyrString *string;
	PyrMethod *meth;
	
	a = g->sp - 1;
	b = g->sp;
	
	// check b is a string
	if (b->utag != tagObj) return errWrongType;
	if (!isKindOf(b->uo,  class_string)) return errWrongType;
	string = b->uos;
	
	gRootParseNode = NULL;
	initParserPool();
		//assert(g->gc->SanityCheck());
	startLexerCmdLine(string->s, string->size);
	compileErrors = 0;
	compilingCmdLine = true;
	gCompilingVMGlobals = g;
	compilingCmdLineErrorWindow = false;
		//assert(g->gc->SanityCheck());
	parseFailed = yyparse();
		//assert(g->gc->SanityCheck());
	if (!parseFailed && gRootParseNode) {
		PyrSlot slotResult;

		meth = GetFunctionCompileContext(g);
		if (!meth) return errFailed;
		
		((PyrBlockNode*)gRootParseNode)->mIsTopLevel = true;
		
		SetNil(&slotResult);
		COMPILENODE(gRootParseNode, &slotResult, true);

		if (slotResult.utag != tagObj 
			|| slotResult.uo->classptr != class_fundef) {
				compileErrors++;
			error("Compile did not return a FunctionDef..\n");
		}
		if (compileErrors) {
			SetNil(a);
		} else {
			PyrBlock *block;	
			PyrClosure *closure;
			
			block = slotResult.uoblk;
			// create a closure
			closure = (PyrClosure*)g->gc->New(2*sizeof(PyrSlot), 0, obj_notindexed, false);
			closure->classptr = class_func;
			closure->size = 2;
			SetObject(&closure->block, block);
			closure->context.ucopy = g->process->interpreter.uoi->context.ucopy;
			SetObject(a, closure);
		}
	} else {
		if (parseFailed) {
			compileErrors++;
			error("Command line parse failed\n");
		} else {
			postfl("<nothing to do>\n");
		}
		SetNil(a);
	}
	finiLexer();
	freeParserPool();
	
	pyr_pool_compile->FreeAll();
	//flushErrors();
	compilingCmdLine = false;
	
	return !(parseFailed || compileErrors) ? errNone : errFailed;
}
#endif

char sCodeStringIn[8192];
char sCodeStringOut[8192];

int prUGenCodeString(struct VMGlobals *g, int numArgsPushed);
int prUGenCodeString(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *aa, *bb, *cc, *dd, *ee;
	char *out = sCodeStringOut;
	char ugenPrefix[16];
	int err;
	
	aa = g->sp - 4;	// code string
	bb = g->sp - 3;	// ugen prefix
	ee = g->sp - 2;	// isDecl
	cc = g->sp - 1;	// input names
	dd = g->sp;		// input value strings
	
	int ugenIndex;
	err = slotIntVal(bb, &ugenIndex);
	if (err) return err;
	if (!isKindOfSlot(cc, class_array) && !isKindOfSlot(cc, class_symbolarray)) return errWrongType;
	if (!isKindOfSlot(dd, class_array)) return errWrongType;
	bool isDecl = IsTrue(ee);
	
	PyrObject *inputNamesObj = cc->uo;
	PyrObject *inputStringsObj = dd->uo;
	
	sprintf(ugenPrefix, "u%d", ugenIndex);
	int ugenPrefixSize = strlen(ugenPrefix);
	PyrString* codeStringObj = aa->uos;
	int codeStringSize = codeStringObj->size;
	if (codeStringSize > 8000) {
		error("input string too int.\n");
		return errFailed;
	}
	memcpy(sCodeStringIn, codeStringObj->s, codeStringSize);
	sCodeStringIn[codeStringSize] = 0;
		
	char* in = sCodeStringIn;
	int c;
	while ((c = *in++) != 0) {
		if (c == '@') {
			if (!isDecl) {
				if (*in != '@') {
					*out++ = 's';
					*out++ = '-';
					*out++ = '>';
				} else in++;
			}
			for (int j=0; j<ugenPrefixSize; ++j) {
				*out++ = ugenPrefix[j];
			}
		} else if (c == '$') {
			char name[64];
			int j=0;
			do {
				c = *in++;
				if (c == 0) break;
				if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9')) {
					--in;
					break;
				}
				name[j++] = c;
			} while (c);
			
			bool found = false;
			int nameSize = j;
			int slotIndex = -1;
			for (int j=0; j<inputNamesObj->size; ++j) {
				PyrSlot inputNameSlot;
				getIndexedSlot(inputNamesObj, &inputNameSlot, j);
				if (!IsSym(&inputNameSlot)) return errWrongType;
				PyrSymbol* inputSym = inputNameSlot.us;
				char *inputName = inputSym->name;
				int inputNameSize = inputSym->length;
				if (inputNameSize == nameSize && strncmp(inputName, name, nameSize)==0) {
					found = true;
					slotIndex = j;
					break;
				}
			}
			if (slotIndex >= 0) {
				PyrSlot *inputStringSlot = inputStringsObj->slots + slotIndex;
				if (!isKindOfSlot(inputStringSlot, class_string)) return errWrongType;
				PyrString *inputStringObj = inputStringSlot->uos;
				char *input = inputStringObj->s;
				int inputStringSize = inputStringObj->size;
				for (int j=0; j<inputStringSize; ++j) {
					*out++ = input[j];
				}
			} else {
				*out++ = '?';	*out++ = '?';
				for (int j=0; j<nameSize; ++j) {
					*out++ = name[j];
				}
				*out++ = '?';	*out++ = '?';
			}
		} else {
			*out++ = c;
		}
		if (out - sCodeStringOut > 8000) {
			*out++ = '\n';
			*out++ = '.';
			*out++ = '.';
			*out++ = '.';
			*out++ = '\n';
			break;
		}
	}
	*out++ = 0;
	PyrString* outString = newPyrString(g->gc, sCodeStringOut, 0, true);
	SetObject(aa, outString);

	return errNone;
}



/*void threadSanity(VMGlobals *g, PyrThread *thread);
void threadSanity(VMGlobals *g, PyrThread *thread)
{
	int state;
	g->gc->numToScan = 1000000;
	doGC(g, 0);
	assert(g->gc->SanityCheck());
	
	state = thread->state.ui;
	if (state == tYield) {
		if (!IsObj(&thread->method)) { error("thread method not an Object\n"); }
		else if (!isKindOf(thread->method.uo, class_method)) { error("thread method not a Method\n"); }
		else if (thread->method.uo->gc_color == gcColor.gcFree) { error("thread method is FREE\n"); }
		
		if (!IsObj(&thread->block)) { error("thread block not an Object\n"); }
		else if (!isKindOf(thread->block.uo, class_func)) { error("thread block not a Function\n"); }
		else if (thread->block.uo->gc_color == gcColor.gcFree) { error("thread block is FREE\n"); }

		if (IsObj(&thread->receiver) && thread->receiver.uo->gc_color == gcColor.gcFree) 
			{ error("thread receiver is FREE\n"); }

		FrameSanity(thread->frame.uof);
		
		oldthread->method.uom = g->method;
		oldthread->block.uoblk = g->block;
		SetObject(&oldthread->frame, g->frame);
		oldthread->ip.ui = (int)g->ip;
		oldthread->sp.ui = (int)g->sp;


	} else if (state == tInit) {
	} else {
		postfl("bad state\n");
	}
}*/


PyrSymbol *s_prready;
PyrSymbol *s_prrunnextthread;

void switchToThread(VMGlobals *g, PyrThread *newthread, int oldstate, int *numArgsPushed);
void switchToThread(VMGlobals *g, PyrThread *newthread, int oldstate, int *numArgsPushed)
{
	PyrThread *oldthread;
	PyrGC *gc;
	PyrFrame *frame;
	
	g->tailCall = 0; // ?? prevent a crash. is there a way to allow a TCO ?
	
	oldthread = g->thread;
	if (newthread == oldthread) return;
	//postfl("->switchToThread %d %08X -> %08X\n", oldstate, oldthread, newthread);
	//post("->switchToThread from %s-%s\n", g->method->ownerclass.uoc->name.us->name, g->method->name.us->name);
	//post("->stack %08X  g->sp %08X [%d]  g->top %08X [%d]\n", 
	//	g->gc->Stack()->slots, g->sp, g->sp - g->gc->Stack()->slots, g->top, g->top - g->gc->Stack()->slots);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "switchToThreadA");
	//gcDumpInfo(g->gc);
	gc = g->gc;

        // save environment in oldthread
        PyrObject* objClassVars = g->classvars[class_object->classIndex.ui].uo;
        PyrSlot* currentEnvironmentSlot = objClassVars->slots + 1;
        oldthread->environment.ucopy = currentEnvironmentSlot->ucopy;
        gc->GCWrite(oldthread, currentEnvironmentSlot);

	oldthread->state.ui = oldstate;
	
	if (oldstate == tDone) {
		SetObject(&oldthread->stack, gc->Stack());
		gc->ToWhite(gc->Stack());
		gc->Stack()->size = 0;
		gc->GCWrite(oldthread, gc->Stack());
		SetNil(&oldthread->method);
		SetNil(&oldthread->block);
		SetNil(&oldthread->receiver);
		SetNil(&oldthread->frame);
		SetInt(&oldthread->ip, 0);
		SetInt(&oldthread->sp, 0);
		SetInt(&oldthread->numArgsPushed, 0);
		SetInt(&oldthread->numpop, 0);
		SetNil(&oldthread->parent);
	} else if (oldstate == tInit) {
		SetObject(&oldthread->stack, gc->Stack());
		gc->ToWhite(gc->Stack());
		gc->Stack()->size = 0;
		gc->GCWrite(oldthread, gc->Stack());
		SetNil(&oldthread->method);
		SetNil(&oldthread->block);
		SetNil(&oldthread->receiver);
		SetNil(&oldthread->frame);
		SetInt(&oldthread->ip, 0);
		SetInt(&oldthread->sp, 0);
		SetInt(&oldthread->numArgsPushed, 0);
		SetInt(&oldthread->numpop, 0);
		SetNil(&oldthread->parent);
	} else {
		// save old thread's state
		SetObject(&oldthread->stack, gc->Stack());
		gc->ToWhite(gc->Stack());
		gc->Stack()->size = g->sp - gc->Stack()->slots + 1;
		//post("else %08X %08X\n", oldthread->stack.uo, gc->Stack());
	
		SetObject(&oldthread->method, g->method);
		SetObject(&oldthread->block, g->block);
		SetObject(&oldthread->frame, g->frame);
		oldthread->ip.ui = (int)g->ip;
		oldthread->sp.ui = (int)g->sp;
		oldthread->receiver.ucopy = g->receiver.ucopy;
		oldthread->numArgsPushed.ui = *numArgsPushed;
		oldthread->numpop.ui = g->numpop;
		
		// these are perhaps unecessary because a thread may not 
		// legally block within a C primitive
		oldthread->returnLevels.ui = g->returnLevels;
			
		//gc->ToGrey(oldthread);
		if (gc->ObjIsBlack(oldthread)) {
			gc->GCWriteBlack(gc->Stack());
			gc->GCWriteBlack(g->method);
			gc->GCWriteBlack(g->block);
			
			frame = oldthread->frame.uof;
			gc->GCWriteBlack(frame);
			
			gc->GCWriteBlack(&g->receiver);
		}
	}
	
	// restore new thread's state
	g->thread = newthread;
	SetObject(&g->process->curThread, newthread);
	gc->GCWrite(g->process, newthread);

	gc->SetStack(newthread->stack.uo);
	gc->ToBlack(gc->Stack());
	SetNil(&newthread->stack);
	
	g->method = newthread->method.uom;
	g->block = newthread->block.uoblk;
	g->frame = newthread->frame.uof;
	g->ip = (unsigned char *)newthread->ip.ui;
	g->sp = (PyrSlot*)newthread->sp.ui;
	g->receiver.ucopy = newthread->receiver.ucopy;
	
	g->rgen = (RGen*)(newthread->randData.uo->slots);

	*numArgsPushed = newthread->numArgsPushed.ui;
	
	// these are perhaps unecessary because a thread may not 
	// legally block within a C primitive
	g->numpop = newthread->numpop.ui;
	g->returnLevels = newthread->returnLevels.ui;
	
	//if (g->ip == (unsigned char *)-1) g->returnLevels = 1;
	//else g->execMethod = 1; //??
	g->execMethod = 99;
	
	//post("switchToThread ip %08X\n", g->ip);
	//post("switchToThread newthread->ip.ui %d\n", newthread->ip.ui);
	//post("switchToThread oldthread->ip.ui %d\n", oldthread->ip.ui);
	//post("switchToThread returnLevels %d\n", g->returnLevels);
	
	// wipe out values which will become stale as new thread executes:
	SetNil(&newthread->method);
	SetNil(&newthread->block);
	SetNil(&newthread->frame);
	newthread->ip.ui = 0;
	newthread->sp.ui = 0;
	SetNil(&newthread->receiver);
	
	newthread->state.ui = tRunning;


        // set new environment
        currentEnvironmentSlot->ucopy = g->thread->environment.ucopy;
        g->gc->GCWrite(objClassVars, currentEnvironmentSlot);

	//post("old thread %08X stack %08X\n", oldthread, oldthread->stack.uo);
	//post("new thread %08X stack %08X\n", g->thread, g->thread->stack.uo);
	//post("main thread %08X stack %08X\n", g->process->mainThread.uot, g->process->mainThread.uot->stack.uo);
	
	//postfl("<-switchToThread\n");
	//post("<-stack %08X  g->sp %08X [%d]  g->top %08X [%d]\n", 
	//	g->gc->Stack()->slots, g->sp, g->sp - g->gc->Stack()->slots, g->top, g->top - g->gc->Stack()->slots);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "switchToThreadB");
	//post("switchToThread ip2 %08X\n", g->ip);
}

#if 0
void pushThread(VMGlobals *g, PyrObject *obj, PyrSlot *head, PyrThread* thread);
void pushThread(VMGlobals *g, PyrObject *obj, PyrSlot *head, PyrThread* thread)
{
	PyrSlot *tail = head + 1;
	
	//post("->pushThread\n");
	tail = head+1;
	if (NotNil(tail)) {
		SetObject(&tail->uot->nextThread, thread);
		g->gc->GCWrite(tail->uo, thread);
	} else {
		SetObject(head, thread);
		g->gc->GCWrite(obj, thread);
	}
	SetObject(tail, thread);
	g->gc->GCWrite(obj, thread);
}

PyrThread* popThread(VMGlobals *g, PyrObject *obj, PyrSlot *head);
PyrThread* popThread(VMGlobals *g, PyrObject *obj, PyrSlot *head)
{
	PyrSlot *tail = head + 1;
	PyrThread* thread;
	
	//post("->popThread\n");
	thread = head->uot;
	tail = head+1;
	if (tail->uot == thread) {
		SetNil(head);
		SetNil(tail);
	} else {
		head->ucopy = thread->nextThread.ucopy;
		g->gc->GCWrite(obj, head->uo);
	}
	return thread;
}
#endif

void initPyrThread(VMGlobals *g, PyrThread *thread, PyrSlot *func, int stacksize, PyrInt32Array* rgenArray, 
	double beats, double seconds, PyrSlot* clock, bool collect);
void initPyrThread(VMGlobals *g, PyrThread *thread, PyrSlot *func, int stacksize, PyrInt32Array* rgenArray, 
	double beats, double seconds, PyrSlot* clock, bool collect)
{
	PyrObject *array;
	PyrGC* gc = g->gc;
        
	thread->func.ucopy = func->ucopy;
	gc->GCWrite(thread, func);

	SetInt(&thread->stackSize, stacksize);
	
	array = newPyrArray(gc, stacksize, 0, collect);
	SetObject(&thread->stack, array);
	gc->GCWrite(thread, array);
	thread->state.ui = tInit;
		
	SetObject(&thread->randData, rgenArray);
	gc->GCWrite(thread, rgenArray);

	thread->beats.uf = beats;
	thread->seconds.uf = seconds;
	
	if (IsNil(clock)) {
		SetObject(&thread->clock, s_systemclock->u.classobj); 
	} else {
		thread->clock.ucopy = clock->ucopy; 
		gc->GCWrite(thread, clock);
	}
        
	PyrSlot* currentEnvironmentSlot = g->classvars[class_object->classIndex.ui].uo->slots + 1;
        thread->environment.ucopy = currentEnvironmentSlot->ucopy;
        gc->GCWrite(thread, currentEnvironmentSlot);
}

extern PyrSymbol *s_prstart;

int prThreadInit(struct VMGlobals *g, int numArgsPushed);
int prThreadInit(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, *c;
	int stacksize, err;
	PyrThread *thread;
	
	//postfl("->prThreadInit\n");
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prThreadInit");
	a = g->sp - 2;	// thread
	b = g->sp - 1;	// function
	c = g->sp;		// stacksize
	
	thread = a->uot;
	
	if (b->utag != tagObj || !isKindOf(b->uo, class_func)) {
		error("Thread function arg not a Function.\n");
		return errWrongType;
	}
	
	err = slotIntVal(c, &stacksize);
	if (err) return err;
	
	initPyrThread(g, thread, b, stacksize, (PyrInt32Array*)(g->thread->randData.uo), 
		g->thread->beats.uf, g->thread->seconds.uf, &g->thread->clock, true);
	
	//postfl("<-prThreadInit\n");
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "<prThreadInit");
	return errNone;
}

int prThreadRandSeed(struct VMGlobals *g, int numArgsPushed);
int prThreadRandSeed(struct VMGlobals *g, int numArgsPushed)
{	
	PyrSlot *a = g->sp - 1;	// thread
	PyrSlot *b = g->sp;		// rand seed
	
	PyrThread *thread = a->uot;

	int32 seed;
	int err = slotIntVal(b, &seed);
	if (err) return err;

	PyrInt32Array *rgenArray = newPyrInt32Array(g->gc, 4, 0, true);
	rgenArray->size = 4;
	((RGen*)(rgenArray->i))->init(seed);

	if (thread == g->thread) {
		g->rgen = (RGen*)(rgenArray->i);
	}
	SetObject(&thread->randData, rgenArray);
	g->gc->GCWrite(thread, rgenArray);
	return errNone;
}

#if 0
int32 timeseed();

int transformMainThreadToRoutine(VMGlobals *g)
{
	PyrProcess* process = g->process;
	if (g->thread != process->mainThread.uot) return errFailed;
	//if (g->thread != process->curThread.uot) return errFailed;
	
	PyrThread* curthread = (PyrThread*)process->mainThread.uo;
	
	// create a new main thread
	PyrThread* newthread = (PyrThread*)instantiateObject(g->gc, class_thread, 0, true, false);
	
	PyrInt32Array *rgenArray = newPyrInt32Array(g->gc, 4, 0, false);
	rgenArray->size = 4;
	((RGen*)(rgenArray->i))->init(timeseed());

	PyrSlot clockSlot;
	SetObject(&clockSlot, s_systemclock->u.classobj);
	initPyrThread(g, newthread, &o_nil, EVALSTACKDEPTH, rgenArray, 0., 0., &clockSlot, false);
	newthread->sp.ui = (int)newthread->stack.uo->slots - 1;
	SetObject(&process->mainThread, newthread);
	g->gc->GCWrite(process, newthread);

	curthread->classptr = class_routine;
	PyrSlot *cmdFunc = &process->interpreter.uoi->cmdFunc;
	curthread->func.ucopy = cmdFunc->ucopy;
	g->gc->GCWrite(curthread, cmdFunc);
	
	return errNone;
}

void schedAdd(VMGlobals *g, PyrObject* inQueue, double inSeconds, PyrSlot* inTask);
#endif


int prRoutineYield(struct VMGlobals *g, int numArgsPushed);
int prRoutineYield(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot value;
	
	//postfl("->prRoutineYield %d %08X\n", g->level, g->thread);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineYield");
	//postfl("->numArgsPushed %d\n", numArgsPushed);

	value.ucopy = g->sp->ucopy;

	if (!isKindOf((PyrObject*)g->thread, class_routine)) {
		error ("yield was called outside of a Routine.\n");
		return errFailed;
	}

	//debugf("yield from thread %08X to parent %08X\n", g->thread, g->thread->parent.uot);
	switchToThread(g, g->thread->parent.uot, tYieldToParent, &numArgsPushed);

	// on the other side of the looking glass, put the yielded value on the stack as the result..
	(g->sp - numArgsPushed + 1)->ucopy = value.ucopy;

	//postfl("<-numArgsPushed %d\n", numArgsPushed);
	//postfl("<-prRoutineYield %d\n", g->level);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "<prRoutineYield");
	return errNone;
}

int prRoutineAlwaysYield(struct VMGlobals *g, int numArgsPushed);
int prRoutineAlwaysYield(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot value;
	
	//postfl("->prRoutineAlwaysYield ip %08X  rl %d\n", g->ip, g->returnLevels);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineAlwaysYield");
	if (!isKindOf((PyrObject*)g->thread, class_routine)) {
		error ("alwaysYield was called outside of a Routine.\n");
		return errFailed;
	}
	
	value.ucopy = g->sp->ucopy;
	g->thread->terminalValue.ucopy = value.ucopy;
	g->gc->GCWrite(g->thread, g->sp);
	
	//post("alwaysYield from thread %08X to parent %08X\n", g->thread, g->thread->parent.uot);
	switchToThread(g, g->thread->parent.uot, tDone, &numArgsPushed);
	
	// on the other side of the looking glass, put the yielded value on the stack as the result..
	(g->sp - numArgsPushed + 1)->ucopy = value.ucopy;
	
	//postfl("<-prRoutineAlwaysYield ip %08X  rl %d\n", g->ip, g->returnLevels);
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "<prRoutineAlwaysYield");
	return errNone;
}

int prRoutineResume(struct VMGlobals *g, int numArgsPushed);
int prRoutineResume(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, threadSlot, value;
	PyrThread *thread;
	int state;
	
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineResume");
	a = g->sp - 1;
	b = g->sp;
	
	thread = a->uot;
	state = thread->state.ui;
	//postfl("->prRoutineResume %d\n", state);
	if (state == tInit) {
		threadSlot.ucopy = a->ucopy;
		value.ucopy = b->ucopy;

	//post("g->thread %08X\n", g->thread);
	//post("thread %08X\n", thread);
		SetObject(&thread->parent, g->thread);
		g->gc->GCWrite(thread, g->thread);
                		
		thread->beats.uf = g->thread->beats.uf;
		thread->seconds.uf = g->thread->seconds.uf;
		thread->clock.ucopy = g->thread->clock.ucopy;
 		g->gc->GCWrite(thread, &g->thread->clock);

		//postfl("start into thread %08X from parent %08X\n", thread, g->thread);
		switchToThread(g, thread, tYieldToChild, &numArgsPushed);
		               
		// set stack
		//post("set stack %08X %08X\n", g->sp, g->gc->Stack()->slots - 1);
		g->sp = g->gc->Stack()->slots - 1;
		g->receiver.ucopy = (++g->sp)->uf = threadSlot.ucopy;
		(++g->sp)->ucopy = value.ucopy;
		
		sendMessage(g, s_prstart, 2);
	} else if (state == tYieldToParent) {

		SetObject(&thread->parent, g->thread);
		g->gc->GCWrite(thread, g->thread);
		
		thread->beats.uf = g->thread->beats.uf;
		thread->seconds.uf = g->thread->seconds.uf;
		thread->clock.ucopy = g->thread->clock.ucopy;
		g->gc->GCWrite(thread, &g->thread->clock);

		value.ucopy = b->ucopy;
	//debugf("resume into thread %08X from parent %08X\n", thread, g->thread);
		switchToThread(g, thread, tYieldToChild, &numArgsPushed);
		// on the other side of the looking glass, put the yielded value on the stack as the result..
		(g->sp - numArgsPushed + 1)->ucopy = value.ucopy;
	} else if (state == tDone) {
		a->ucopy = thread->terminalValue.ucopy;
	} else if (state == tRunning) {
		error("Tried to resume the running thread.\n");
		return errFailed;
	} else {
		error("Thread in strange state: %d\n", state);
		return errFailed;
	}
	//postfl("<-prRoutineResume\n");
	//CallStackSanity(g);
	return errNone;
}

int prRoutineReset(struct VMGlobals *g, int numArgsPushed);
int prRoutineReset(struct VMGlobals *g, int numArgsPushed)
{
	PyrThread *thread;
	int state;
	
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineReset");
	thread = g->sp->uot;
	state = thread->state.ui;
	//post("->prRoutineReset %d\n", state);
	if (state == tYieldToParent || state == tYieldToChild) {
		thread->state.ui = tInit;
		thread->stack.uo->size = 0;
		SetNil(&thread->method);
		SetNil(&thread->block);
		SetNil(&thread->receiver);
		SetNil(&thread->frame);
		SetInt(&thread->ip, 0);
		SetInt(&thread->sp, 0);
		SetInt(&thread->numArgsPushed, 0);
		SetInt(&thread->numpop, 0);
		SetNil(&thread->parent);
	} else if (state == tDone) {
		PyrObject *array;
		thread->state.ui = tInit;
		//array = newPyrArray(g->gc, thread->stackSize.ui, 0, true);
		//SetObject(&thread->stack, array);
		//g->gc->GCWrite(thread, array);
		thread->stack.uo->size = 0;
		SetNil(&thread->method);
		SetNil(&thread->block);
		SetNil(&thread->receiver);
		SetNil(&thread->frame);
		SetInt(&thread->ip, 0);
		SetInt(&thread->sp, 0);
		SetInt(&thread->numArgsPushed, 0);
		SetInt(&thread->numpop, 0);
		SetNil(&thread->parent);
	} else if (state == tInit) {
		// do nothing
	} else if (state == tRunning) {
		error("A Routine cannot reset itself except by yieldAndReset.\n");
		return errFailed;
	} else {
		error("Routine in unknown state. %d\n", state);
		return errFailed;
	}
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "<prRoutineReset");
	return errNone;
}

int prRoutineStop(struct VMGlobals *g, int numArgsPushed);
int prRoutineStop(struct VMGlobals *g, int numArgsPushed)
{
	PyrThread *thread;
	int state;
	//post("->prRoutineStop\n");
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineStop");
	
	thread = g->sp->uot;
	state = thread->state.ui;


	if (state == tYieldToParent || state == tYieldToChild || state == tInit) {
		SetNil(&g->thread->terminalValue);
		thread->state.ui = tDone;
		thread->stack.uo->size = 0;
	} else if (state == tDone) {
		// do nothing
	} else if (state == tRunning) {
		error("Do not call .stop from within the Routine.\n");
		post("A Routine should stop itself using nil.alwaysYield.\n");
		return errFailed;
	} else {
		error("Routine in unknown state. %d\n", state);
		return errFailed;
	}
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "<prRoutineStop");
	return errNone;
}


int prRoutineYieldAndReset(struct VMGlobals *g, int numArgsPushed);
int prRoutineYieldAndReset(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a, *b, value;
	int state;
	
	//post("->prRoutineYieldAndReset\n");
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineYieldAndReset");
	a = g->sp - 1;
	b = g->sp;
	
	if (!isKindOf((PyrObject*)g->thread, class_routine)) {
		error ("yieldAndReset was called outside of a Routine.\n");
		return errFailed;
	}
	/*if (!g->thread->parent.uot) {
		error ("yieldAndReset was called from a thread with no parent.\n");
		return errFailed;
	}*/
	value.ucopy = a->ucopy;

	if (IsFalse(b)) state = tYieldToParent;
	else state = tInit;
	switchToThread(g, g->thread->parent.uot, state, &numArgsPushed);
	// on the other side of the looking glass, put the yielded value on the stack as the result..
	(g->sp - numArgsPushed + 1)->ucopy = value.ucopy;

	//post("<-prRoutineYieldAndReset\n");
	//assert(g->gc->SanityCheck());
	//CallStackSanity(g, "prRoutineYieldAndReset");
	return errNone;
}


bool gBlork = false;

int prBlork(struct VMGlobals *g, int numArgsPushed);
int prBlork(struct VMGlobals *g, int numArgsPushed)
{
	PyrSlot *a;

	a = g->sp;
	if (IsTrue(a)) gBlork = true;
	else gBlork = false;
	
	return errNone;
}
	

#define PRIMGROWSIZE 480
PrimitiveTable gPrimitiveTable;

void initPrimitiveTable()
{
	int i;
	gPrimitiveTable.maxsize = PRIMGROWSIZE;
	gPrimitiveTable.size = 0;
	// pyrmalloc: 
	// lifetime: runtime. primitives are reloaded when library is compiled.
	gPrimitiveTable.table = (PrimitiveDef*)pyr_pool_runtime->Alloc(gPrimitiveTable.maxsize * sizeof(PrimitiveDef));
	MEMFAIL(gPrimitiveTable.table);
	for (i=0; i<gPrimitiveTable.maxsize; ++i) {
		gPrimitiveTable.table[i].func = undefinedPrimitive;
		gPrimitiveTable.table[i].name = s_none;
		gPrimitiveTable.table[i].base = 0;
		gPrimitiveTable.table[i].numArgs = 0;
		gPrimitiveTable.table[i].varArgs = 0;
		gPrimitiveTable.table[i].keyArgs = 0;
	}
}

void growPrimitiveTable(int newsize)
{
	PrimitiveDef *oldtable;
	int i, oldsize;
	//postfl("growPrimitiveTable %d %d\n", oldsize, newsize);
	oldtable = gPrimitiveTable.table;
	oldsize = gPrimitiveTable.maxsize;
	gPrimitiveTable.maxsize = newsize;
	// pyrmalloc: 
	// lifetime: runtime. primitives are reloaded when library is compiled.
	gPrimitiveTable.table = (PrimitiveDef*)pyr_pool_runtime->Alloc(gPrimitiveTable.maxsize * sizeof(PrimitiveDef));
	MEMFAIL(gPrimitiveTable.table);
	memcpy(gPrimitiveTable.table, oldtable, oldsize * sizeof(PrimitiveDef));
	for (i=oldsize; i<gPrimitiveTable.maxsize; ++i) {
		gPrimitiveTable.table[i].func = undefinedPrimitive;
		gPrimitiveTable.table[i].name = s_none;
		gPrimitiveTable.table[i].base = 0;
		gPrimitiveTable.table[i].numArgs = 0;
		gPrimitiveTable.table[i].varArgs = 0;
		gPrimitiveTable.table[i].keyArgs = 0;
	}
	pyr_pool_runtime->Free(oldtable);
}

int definePrimitive(int base, int index, char *name, PrimitiveHandler handler,
	int numArgs, int varArgs)
{
	int tableIndex;
	PyrSymbol *sym;
	
	if (name[0] != '_') {
		error("*** Primitive Name must begin with an underscore ***\n");
		postfl("name: '%s' index: %d\n", name, index);
		return -1;
	}
	tableIndex = base + index;
	if (tableIndex < 0) {
		error("*** Negative Primitive Index ***\n");
		postfl("name: '%s' index: %d\n", name, tableIndex);
		return -1;
	}
	if (tableIndex >= gPrimitiveTable.maxsize) {
		growPrimitiveTable(tableIndex + PRIMGROWSIZE);
	}
	if (gPrimitiveTable.table[tableIndex].func != undefinedPrimitive) {
		error("*** Duplicate Primitive Index ***\n");
		postfl("name: '%s' index: %d\n", name, tableIndex);
		return -1;
	}
	sym = getsym(name);
	gPrimitiveTable.table[tableIndex].func = handler;
	gPrimitiveTable.table[tableIndex].name = sym;
	gPrimitiveTable.table[tableIndex].base = base;
	gPrimitiveTable.table[tableIndex].numArgs = numArgs;
	gPrimitiveTable.table[tableIndex].varArgs = varArgs;
	gPrimitiveTable.table[tableIndex].keyArgs = 0;
	if (tableIndex > gPrimitiveTable.size) gPrimitiveTable.size = tableIndex;
	sym->u.index = tableIndex;
	return tableIndex;
}

int definePrimitiveWithKeys(int base, int index, char *name, 
	PrimitiveHandler handler, PrimitiveWithKeysHandler keyhandler,
	int numArgs, int varArgs)
{
	int tableIndex;
	PyrSymbol *sym;
	
	if (name[0] != '_') {
		error("*** Primitive Name must begin with an underscore ***\n");
		postfl("name: '%s' index: %d\n", name, index);
		return -1;
	}
	tableIndex = base + index;
	if (tableIndex < 0) {
		error("*** Negative Primitive Index ***\n");
		postfl("name: '%s' index: %d\n", name, tableIndex);
		return -1;
	}
	if (tableIndex+1 >= gPrimitiveTable.maxsize) {
		growPrimitiveTable(tableIndex + PRIMGROWSIZE);
	}
	if (gPrimitiveTable.table[tableIndex].func != undefinedPrimitive) {
		error("*** Duplicate Primitive Index ***\n");
		postfl("name: '%s' index: %d\n", name, tableIndex);
		return -1;
	}
	sym = getsym(name);
	gPrimitiveTable.table[tableIndex].func = handler;
	gPrimitiveTable.table[tableIndex].name = sym;
	gPrimitiveTable.table[tableIndex].base = base;
	gPrimitiveTable.table[tableIndex].numArgs = numArgs;
	gPrimitiveTable.table[tableIndex].varArgs = varArgs;
	gPrimitiveTable.table[tableIndex].keyArgs = 1;
	sym->u.index = tableIndex;

	tableIndex++;
	gPrimitiveTable.table[tableIndex].func = (PrimitiveHandler)keyhandler;
	gPrimitiveTable.table[tableIndex].name = sym;
	gPrimitiveTable.table[tableIndex].base = base;
	gPrimitiveTable.table[tableIndex].numArgs = numArgs;
	gPrimitiveTable.table[tableIndex].varArgs = varArgs;
	gPrimitiveTable.table[tableIndex].keyArgs = 1;
	if (tableIndex > gPrimitiveTable.size) gPrimitiveTable.size = tableIndex;
	return tableIndex;
}

int nextPrimitiveIndex()
{
	return gPrimitiveTable.size + 1;
}


void doPrimitive(VMGlobals* g, PyrMethod* meth, int numArgsPushed)
{


#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
	
	//post("doPrimitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
        //printf("doPrimitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);

	PyrMethodRaw *methraw = METHRAW(meth);
	int primIndex = methraw->specialIndex;
	
	PrimitiveDef *def = gPrimitiveTable.table + primIndex;
	int numArgsNeeded = def->numArgs;
	int diff = numArgsNeeded - numArgsPushed;
	
	if (diff != 0) { // incorrect num of args
		if (diff > 0) {  // not enough args
			double* pslot = (double*)(g->sp);
			double* qslot = (double*)(meth->prototypeFrame.uo->slots + numArgsPushed - 1);
			for (int m=0; m<diff; ++m) *++pslot = *++qslot;
			
			g->sp += diff;
		} else if (def->varArgs) { // has var args
			numArgsNeeded = numArgsPushed;
		} else {
			g->sp += diff; // remove excess args
		}
	}
	g->numpop = numArgsNeeded - 1;
	g->primitiveIndex = primIndex - def->base;
	g->primitiveMethod = meth;
	g->args = g->sp - numArgsNeeded;
	int err;
	try {
#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
		err = (*def->func)(g, numArgsNeeded);
#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
	} catch (std::exception& ex) {
		post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		error(ex.what());
		err = errException;
	} catch (...) {
		post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		err = errException;
	}
	if (err <= errNone) g->sp -= g->numpop;
	else {
		//post("primitive failed %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
		SetInt(&g->thread->primitiveIndex, methraw->specialIndex);
		SetInt(&g->thread->primitiveError, err);
		executeMethod(g, meth, numArgsNeeded);
	}
#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
}

void doPrimitiveWithKeys(VMGlobals* g, PyrMethod* meth, int allArgsPushed, int numKeyArgsPushed)
{
	int i, j, m, diff, err;
	double *pslot, *qslot;
	int numArgsNeeded, numArgsPushed;
	
#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
	//post("doPrimitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
        //printf("doPrimitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);

	PyrMethodRaw *methraw = METHRAW(meth);
	int primIndex = methraw->specialIndex;
	PrimitiveDef *def = gPrimitiveTable.table + primIndex;
	g->primitiveIndex = primIndex - def->base;
	g->primitiveMethod = meth;
	
	if (def->keyArgs && numKeyArgsPushed) {
		g->numpop = allArgsPushed - 1;
		try {
			err = ((PrimitiveWithKeysHandler)def[1].func)(g, allArgsPushed, numKeyArgsPushed);
		} catch (std::exception& ex) {
			post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
			error(ex.what());
			err = errException;
		} catch (...) {
			post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
			err = errException;
		}
		if (err <= errNone) g->sp -= g->numpop;
		else {
			//post("primerr %d\n", err);
			SetInt(&g->thread->primitiveIndex, methraw->specialIndex);
			SetInt(&g->thread->primitiveError, err);
			executeMethodWithKeys(g, meth, allArgsPushed, numKeyArgsPushed);
		}
	} else {
		numArgsNeeded = def->numArgs;
		numArgsPushed = allArgsPushed - (numKeyArgsPushed << 1);
		
		if (numKeyArgsPushed) {
			// evacuate keyword args to separate area
			pslot = (double*)(keywordstack + (numKeyArgsPushed<<1));
			qslot = (double*)(g->sp + 1);
			for (m=0; m<numKeyArgsPushed; ++m) {
				*--pslot = *--qslot;
				*--pslot = *--qslot;
			}
		}
		
		diff = numArgsNeeded - numArgsPushed;
		if (diff != 0) { // incorrect num of args
			if (diff > 0) {  // not enough args
				g->sp += numArgsNeeded - allArgsPushed; // remove excess args
				pslot = (double*)(g->sp - diff);
				qslot = (double*)(meth->prototypeFrame.uo->slots + numArgsPushed - 1);
				for (m=0; m<diff; ++m) *++pslot = *++qslot;
			} else if (def->varArgs) { // has var args
				numArgsNeeded = numArgsPushed;
				g->sp += numArgsNeeded - allArgsPushed; // remove excess args
			} else {
				g->sp += numArgsNeeded - allArgsPushed; // remove excess args
			}
		}

		// do keyword lookup:
		if (numKeyArgsPushed && methraw->posargs) {
			PyrSymbol **name0, **name;
			PyrSlot *key, *vars;
			name0 = meth->argNames.uosym->symbols + 1;
			key = keywordstack;
			vars = g->sp - numArgsNeeded + 1;
			for (i=0; i<numKeyArgsPushed; ++i, key+=2) {
				name = name0;
				for (j=1; j<methraw->posargs; ++j, ++name) {
					if (*name == key->us) {
						vars[j].ucopy = key[1].ucopy;
						goto found;
					}
				}
				if (gKeywordError) {
					post("WARNING: keyword arg '%s' not found in call to %s-%s\n",
						key->us->name, meth->ownerclass.uoc->name.us->name, meth->name.us->name);
				}
				found: ;
			}
		}
		g->numpop = numArgsNeeded - 1;
		try {
			err = (*def->func)(g, numArgsNeeded);
		} catch (std::exception& ex) {
			post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
			error(ex.what());
			err = errException;
		} catch (...) {
			post("caught exception in primitive %s-%s\n", meth->ownerclass.uoc->name.us->name, meth->name.us->name);
			err = errException;
		}
		if (err <= errNone) g->sp -= g->numpop;
		else {
			//post("primerr %d\n", err);
			SetInt(&g->thread->primitiveIndex, methraw->specialIndex);
			SetInt(&g->thread->primitiveError, err);
			executeMethod(g, meth, numArgsNeeded);
		}
	}
#if SANITYCHECK
	g->gc->SanityCheck();
#endif	
}

void initPrimitives()
{
	int base, index;
	
	initPrimitiveTable();
	
	// unary operators
	base = nextPrimitiveIndex();
	definePrimitive(base, opNeg, "_Neg", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opBitNot, "_BitNot", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opAbs, "_Abs", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opAsFloat, "_AsFloat", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opAsInt, "_AsInt", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCeil, "_Ceil", doSpecialUnaryArithMsg, 1, 0);			// 5
	definePrimitive(base, opFloor, "_Floor", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opFrac, "_Frac", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opSign, "_Sign", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opSquared, "_Squared", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCubed, "_Cubed", doSpecialUnaryArithMsg, 1, 0);			//10
	definePrimitive(base, opSqrt, "_Sqrt", doSpecialUnaryArithMsg, 1, 0);		
	definePrimitive(base, opExp, "_Exp", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opRecip, "_Recip", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opMIDICPS, "_MIDICPS", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCPSMIDI, "_CPSMIDI", doSpecialUnaryArithMsg, 1, 0);		//15
	
	definePrimitive(base, opMIDIRatio, "_MIDIRatio", doSpecialUnaryArithMsg, 1, 0);		
	definePrimitive(base, opRatioMIDI, "_RatioMIDI", doSpecialUnaryArithMsg, 1, 0);		
	definePrimitive(base, opDbAmp, "_DbAmp", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opAmpDb, "_AmpDb", doSpecialUnaryArithMsg, 1, 0);	
	definePrimitive(base, opOctCPS, "_OctCPS", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCPSOct, "_CPSOct", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opLog, "_Log", doSpecialUnaryArithMsg, 1, 0);				
	definePrimitive(base, opLog2, "_Log2", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opLog10, "_Log10", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opSin, "_Sin", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCos, "_Cos", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opTan, "_Tan", doSpecialUnaryArithMsg, 1, 0);				
	definePrimitive(base, opArcSin, "_ArcSin", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opArcCos, "_ArcCos", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opArcTan, "_ArcTan", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opSinH, "_SinH", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCosH, "_CosH", doSpecialUnaryArithMsg, 1, 0);			
	definePrimitive(base, opTanH, "_TanH", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opRand, "_Rand", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opRand2, "_Rand2", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opLinRand, "_LinRand", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opBiLinRand, "_BiLinRand", doSpecialUnaryArithMsg, 1, 0);

	definePrimitive(base, opSum3Rand, "_Sum3Rand", doSpecialUnaryArithMsg, 1, 0);
//	definePrimitive(base, opExpRand, "_ExpRand", doSpecialUnaryArithMsg, 1, 0);
//	definePrimitive(base, opBiExpRand, "_BiExpRand", doSpecialUnaryArithMsg, 1, 0);
//	definePrimitive(base, opGammaRand, "_GammaRand", doSpecialUnaryArithMsg, 1, 0);
//	definePrimitive(base, opGaussRand, "_GaussRand", doSpecialUnaryArithMsg, 1, 0);
//	definePrimitive(base, opPoiRand, "_PoiRand", doSpecialUnaryArithMsg, 1, 0);

	definePrimitive(base, opDistort, "_Distort", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opSoftClip, "_SoftClip", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opCoin, "_Coin", doSpecialUnaryArithMsg, 1, 0);

	definePrimitive(base, opRectWindow, "_RectWindow", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opHanWindow, "_HanWindow", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opWelchWindow, "_WelchWindow", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opTriWindow, "_TriWindow", doSpecialUnaryArithMsg, 1, 0);

	definePrimitive(base, opSCurve, "_SCurve", doSpecialUnaryArithMsg, 1, 0);
	definePrimitive(base, opRamp, "_Ramp", doSpecialUnaryArithMsg, 1, 0);

	definePrimitive(base, opDigitValue, "_DigitValue", doSpecialUnaryArithMsg, 1, 0);	
	
	
	// binary operators
	base = nextPrimitiveIndex();
	definePrimitive(base, opAdd, "_Add", prAddNum, 2, 0);		
	definePrimitive(base, opSub, "_Sub", prSubNum, 2, 0); 
	definePrimitive(base, opMul, "_Mul", prMulNum, 2, 0); 

	definePrimitive(base, opIDiv, "_IDiv", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opFDiv, "_FDiv", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opMod, "_Mod", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opEQ, "_EQ", prSpecialBinaryArithMsg, 3, 0);		
	definePrimitive(base, opNE, "_NE", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opLT, "_LT", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opGT, "_GT", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opLE, "_LE", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opGE, "_GE", prSpecialBinaryArithMsg, 3, 0);
	//definePrimitive(base, opIdentical, "_Identical", prSpecialBinaryArithMsg, 3, 0);
	//definePrimitive(base, opNotIdentical, "_NotIdentical", prSpecialBinaryArithMsg, 3, 0);	
	
	definePrimitive(base, opMin, "_Min", prSpecialBinaryArithMsg, 3, 0); 
	definePrimitive(base, opMax, "_Max", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opBitAnd, "_BitAnd", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opBitOr, "_BitOr", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opBitXor, "_BitXor", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opLCM, "_LCM", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opGCD, "_GCD", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opRound, "_Round", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opRoundUp, "_RoundUp", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opTrunc, "_Trunc", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opAtan2, "_Atan2", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opHypot, "_Hypot", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opHypotx, "_HypotApx", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opPow, "_Pow", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opShiftLeft, "_ShiftLeft", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opShiftRight, "_ShiftRight", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opUnsignedShift, "_UnsignedShift", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opFill, "_Fill", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opRing1, "_Ring1", prSpecialBinaryArithMsg, 3, 0);	// a * (b + 1) == a * b + a
	definePrimitive(base, opRing2, "_Ring2", prSpecialBinaryArithMsg, 3, 0);	// a * b + a + b
	definePrimitive(base, opRing3, "_Ring3", prSpecialBinaryArithMsg, 3, 0);	// a*a*b
	definePrimitive(base, opRing4, "_Ring4", prSpecialBinaryArithMsg, 3, 0);	// a*a*b - a*b*b
	definePrimitive(base, opDifSqr, "_DifSqr", prSpecialBinaryArithMsg, 3, 0);	// a*a - b*b
	definePrimitive(base, opSumSqr, "_SumSqr", prSpecialBinaryArithMsg, 3, 0);	// a*a + b*b
	definePrimitive(base, opSqrSum, "_SqrSum", prSpecialBinaryArithMsg, 3, 0);	// (a + b)^2
	definePrimitive(base, opSqrDif, "_SqrDif", prSpecialBinaryArithMsg, 3, 0);	// (a - b)^2
	definePrimitive(base, opAbsDif, "_AbsDif", prSpecialBinaryArithMsg, 3, 0);	// abs(a - b)
	definePrimitive(base, opThresh, "_Thresh", prSpecialBinaryArithMsg, 3, 0);	// a * max(0,b)
	definePrimitive(base, opAMClip, "_AMClip", prSpecialBinaryArithMsg, 3, 0);	// a * max(0,b)
	definePrimitive(base, opScaleNeg, "_ScaleNeg", prSpecialBinaryArithMsg, 3, 0);	// a < 0 ? a*b : a
	definePrimitive(base, opClip2, "_Clip2", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opFold2, "_Fold2", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opWrap2, "_Wrap2", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opExcess, "_Excess", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opFirstArg, "_FirstArg", prSpecialBinaryArithMsg, 3, 0);	
	definePrimitive(base, opRandRange, "_RandRange", prSpecialBinaryArithMsg, 3, 0);
	definePrimitive(base, opExpRandRange, "_ExpRandRange", prSpecialBinaryArithMsg, 3, 0);

	// general primitives
	base = nextPrimitiveIndex();
	index = 0;
	definePrimitive(base, index++, "_Halt", haltInterpreter, 1, 0);
	definePrimitive(base, index++, "_InstVarAt", instVarAt, 2, 0);
	definePrimitive(base, index++, "_InstVarPut", instVarPut, 3, 0);
	definePrimitive(base, index++, "_InstVarSize", instVarSize, 1, 0);
	definePrimitive(base, index++, "_ObjectHash", objectHash, 1, 0);
	definePrimitive(base, index++, "_ObjectClass", objectClass, 1, 0);
	definePrimitive(base, index++, "_BasicNew", basicNew, 2, 0);
	definePrimitive(base, index++, "_BasicNewClear", basicNewClear, 2, 0);
	definePrimitive(base, index++, "_BasicNewCopyArgsToInstVars", basicNewCopyArgsToInstanceVars, 1, 1);
	//definePrimitive(base, index++, "_BasicNewCopyArgsByName", basicNewCopyArgsByName, 1, 1);
	
	definePrimitiveWithKeys(base, index, "_FunctionValue", blockValue, blockValueWithKeys, 1, 1);
	index+=2;
	definePrimitiveWithKeys(base, index, "_FunctionValueEnvir", blockValueEnvir, blockValueEnvirWithKeys, 1, 1);
	index+=2;
	
	definePrimitive(base, index++, "_FunctionValueArray", blockValueArray, 1, 1);
	definePrimitive(base, index++, "_FunctionValueArrayEnvir", blockValueArrayEnvir, 1, 1);
	definePrimitive(base, index++, "_FunctionDefAsFunction", prFunctionDefAsFunction, 1, 0);
	definePrimitive(base, index++, "_FunctionDefDumpContexts", prFunctionDefDumpContexts, 1, 0);
	definePrimitive(base, index++, "_FunctionDefIsClosed", prFunctionDefIsClosed, 1, 0);
	definePrimitive(base, index++, "_FunctionDefIsWithinClosed", prFunctionDefIsWithinClosed, 1, 0);
	
	definePrimitive(base, index++, "_ObjectIsKindOf", objectIsKindOf, 2, 0);
	definePrimitive(base, index++, "_ObjectIsMemberOf", objectIsMemberOf, 2, 0);
	definePrimitive(base, index++, "_ObjectDump", objectDump, 1, 0);
	definePrimitive(base, index++, "_TotalFree", prTotalFree, 1, 0);
	definePrimitive(base, index++, "_LargestFreeBlock", prLargestFreeBlock, 1, 0);

	definePrimitive(base, index++, "_GCInfo", dumpGCinfo, 1, 0);
	definePrimitive(base, index++, "_GCDumpGrey", dumpGCdumpGrey, 1, 0);
	definePrimitive(base, index++, "_GCDumpSet", dumpGCdumpSet, 2, 0);
	definePrimitive(base, index++, "_GCSanity", prGCSanity, 1, 0);
#if GCDEBUG
	definePrimitive(base, index++, "_TraceAllPathsTo", prTraceAllPathsTo, 1, 0);
	definePrimitive(base, index++, "_TraceAnyPathsTo", prTraceAnyPathsTo, 1, 0);
	definePrimitive(base, index++, "_TraceAnyPathToAllInstancesOf", prTraceAnyPathToAllInstancesOf, 1, 0);
#endif
	
	definePrimitive(base, index++, "_Identical", objectIdentical, 2, 0);
	definePrimitive(base, index++, "_NotIdentical", objectNotIdentical, 2, 0);	
	definePrimitiveWithKeys(base, index, "_ObjectPerform", objectPerform, objectPerformWithKeys, 2, 1);	
	index+=2;
	definePrimitive(base, index++, "_ObjectPerformList", objectPerformList, 2, 1);	
	definePrimitiveWithKeys(base, index, "_SuperPerform", objectSuperPerform, objectSuperPerformWithKeys, 2, 1);	
	index+=2;
	definePrimitive(base, index++, "_SuperPerformList", objectSuperPerformList, 2, 1);	
	definePrimitive(base, index++, "_ObjectPerformMsg", objectPerformSelList, 2, 0);
	//definePrimitive(base, index++, "_ArrayPerformMsg", arrayPerformMsg, 1, 1);
	definePrimitive(base, index++, "_ObjectString", prObjectString, 1, 0);	
	definePrimitive(base, index++, "_Float_AsStringPrec", prFloat_AsStringPrec, 2, 0);	
	definePrimitive(base, index++, "_ObjectCompileString", prAsCompileString, 1, 0);	
	definePrimitive(base, index++, "_ClassString", prClassString, 1, 0);	
	definePrimitive(base, index++, "_PostString", prPostString, 1, 0);	
	definePrimitive(base, index++, "_PostLine", prPostLine, 1, 0);	
	definePrimitive(base, index++, "_HostDebugger", prDebugger, 1, 0);	
	definePrimitive(base, index++, "_Trace", prTraceOn, 1, 0);
	definePrimitive(base, index++, "_CanCallOS", prCanCallOS, 1, 0);
	definePrimitive(base, index++, "_KeywordError", prKeywordError, 1, 0);

	definePrimitive(base, index++, "_PrimitiveError", prPrimitiveError, 1, 0);	
	definePrimitive(base, index++, "_PrimitiveErrorString", prPrimitiveErrorString, 1, 0);	
	definePrimitive(base, index++, "_DumpStack", prDumpStack, 1, 0);	
	definePrimitive(base, index++, "_DumpDetailedBackTrace", prDumpDetailedBackTrace, 1, 0);	
	definePrimitive(base, index++, "_StackDepth", prStackDepth, 1, 0);	
	definePrimitive(base, index++, "_PrimName", prPrimName, 1, 0);	
	definePrimitive(base, index++, "_ObjectShallowCopy", prObjectShallowCopy, 1, 0);	
	definePrimitive(base, index++, "_ObjectCopyImmutable", prObjectCopyImmutable, 1, 0);	
	definePrimitive(base, index++, "_ObjectCopyRange", prObjectCopyRange, 3, 0);	
	definePrimitive(base, index++, "_ObjectCopySeries", prObjectCopySeries, 4, 0);	
	definePrimitive(base, index++, "_ObjectPointsTo", prObjectPointsTo, 2, 0);	
	definePrimitive(base, index++, "_ObjectRespondsTo", prObjectRespondsTo, 2, 0);	
	definePrimitive(base, index++, "_ObjectIsMutable", prObjectIsMutable, 1, 0);
	definePrimitive(base, index++, "_ObjectIsPermanent", prObjectIsPermanent, 1, 0);
	definePrimitive(base, index++, "_ObjectDeepFreeze", prDeepFreeze, 1, 0);
	definePrimitive(base, index++, "_ObjectDeepCopy", prDeepCopy, 1, 0);

#if !SCPLAYER	
	definePrimitive(base, index++, "_CompileExpression", prCompileString, 2, 0);	
#endif
	definePrimitive(base, index++, "_GetBackTrace", prGetBackTrace, 1, 0);	
	definePrimitive(base, index++, "_DumpBackTrace", prDumpBackTrace, 1, 0);	
	definePrimitive(base, index++, "_DumpByteCodes", prDumpByteCodes, 1, 0);	

	definePrimitive(base, index++, "_AllClasses", prAllClasses, 1, 0);	
	definePrimitive(base, index++, "_DumpClassSubtree", prPostClassTree, 1, 0);	

//	definePrimitive(base, index++, "_TabletTracking", prTabletTracking, 1, 0);	

	definePrimitive(base, index++, "_FunDef_NumArgs", prFunDef_NumArgs, 1, 0);	
	definePrimitive(base, index++, "_FunDef_NumVars", prFunDef_NumVars, 1, 0);	
	definePrimitive(base, index++, "_FunDef_VarArgs", prFunDef_VarArgs, 1, 0);	

	definePrimitive(base, index++, "_Thread_Init", prThreadInit, 3, 0);	
	definePrimitive(base, index++, "_Thread_RandSeed", prThreadRandSeed, 2, 0);	
//	definePrimitive(base, index++, "_ThreadRun", prThreadRun, 2, 0);	
//	definePrimitive(base, index++, "_RunNextThread", prRunNextThread, 1, 0);	
	definePrimitive(base, index++, "_RoutineYield", prRoutineYield, 1, 0);	
	definePrimitive(base, index++, "_RoutineAlwaysYield", prRoutineAlwaysYield, 1, 0);	
	definePrimitive(base, index++, "_RoutineResume", prRoutineResume, 2, 0);	
	definePrimitive(base, index++, "_RoutineReset", prRoutineReset, 1, 0);	
	definePrimitive(base, index++, "_RoutineYieldAndReset", prRoutineYieldAndReset, 2, 0);	
	definePrimitive(base, index++, "_RoutineStop", prRoutineStop, 1, 0);	
	
//	definePrimitive(base, index++, "_IsDemo", prIsDemo, 1, 0);	
	definePrimitive(base, index++, "_Blork", prBlork, 1, 0);	
	definePrimitive(base, index++, "_UGenCodeString", prUGenCodeString, 5, 0);	

	//void initOscilPrimitives();
	//void initControllerPrimitives();
	
	//initOscilPrimitives();
	//initControllerPrimitives();
	initMathPrimitives();
	initSignalPrimitives();
	initArrayPrimitives();
	
void initSymbolPrimitives();
	initSymbolPrimitives();

void initArchiverPrimitives();
	 initArchiverPrimitives();

void initArrayPrimitives();
	 initArrayPrimitives();

void initBitPrimitives();
	 initBitPrimitives();

void initCharPrimitives();
	 initCharPrimitives();

void initFilePrimitives();
	 initFilePrimitives();

void initStringPrimitives();
	 initStringPrimitives();

void initListPrimitives();
	 initListPrimitives();
	
void initUnixPrimitives();
	 initUnixPrimitives();

void init_OSC_primitives();
	 init_OSC_primitives();

/*  these probably should be moved out of the Lang code
into an App init primitives section */
void initGUIPrimitives();
        initGUIPrimitives();

void initSCViewPrimitives();
        initSCViewPrimitives();

void initSchedPrimitives();
        initSchedPrimitives();

void initMIDIPrimitives();
		initMIDIPrimitives();

void initHIDPrimitives();
                initHIDPrimitives();

void initSpeechPrimitives();
		initSpeechPrimitives();     
                
void initCocoaFilePrimitives();
        initCocoaFilePrimitives();

// CR ADDED
void initRendezvousPrimitives();
	initRendezvousPrimitives();
	
#ifdef SCOGL_COMPILE
void initOpenGLPrimitives();
        initOpenGLPrimitives();
#endif

	s_recvmsg = getsym("receiveMsg");
	post("\tNumPrimitives = %d\n", nextPrimitiveIndex());
	
}


void initThreads();
void initThreads()
{
	s_prrunnextthread = getsym("prRunNextThread");
	s_prready = getsym("prReady");
}


