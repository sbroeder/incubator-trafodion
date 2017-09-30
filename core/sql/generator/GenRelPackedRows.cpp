/**********************************************************************
// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@
**********************************************************************/
#include "GroupAttr.h"
#include "AllRelExpr.h"
#include "RelPackedRows.h"
#include "Generator.h"
#include "GenExpGenerator.h"
#include "ExpCriDesc.h"
#include "ComTdbUnPackRows.h"
#include "ex_queue.h"

// PhysUnPackRows::preCodeGen() -------------------------------------------
// Perform local query rewrites such as for the creation and
// population of intermediate tables, for accessing partitioned
// data. Rewrite the value expressions after minimizing the dataflow
// using the transitive closure of equality predicates.
//
// PhysUnPackRows::preCodeGen() - is basically the same as the RelExpr::
// preCodeGen() except that here we replace the VEG references in the
// unPackExpr() and packingFactor(), as well as the selectionPred().
//
// Parameters:
//
// Generator *generator
//    IN/OUT : A pointer to the generator object which contains the state,
//             and tools (e.g. expression generator) to generate code for
//             this node.
//
// ValueIdSet &externalInputs
//    IN    : The set of external Inputs available to this node.
//
//
RelExpr * PhysUnPackRows::preCodeGen(Generator * generator, 
				     const ValueIdSet & externalInputs,
				     ValueIdSet &pulledNewInputs)
{
  if (nodeIsPreCodeGenned())
    return this;

  // Resolve the VEGReferences and VEGPredicates, if any, that appear
  // in the Characteristic Inputs, in terms of the externalInputs.
  //
  getGroupAttr()->resolveCharacteristicInputs(externalInputs);

  // My Characteristic Inputs become the external inputs for my children.
  //
  ValueIdSet childPulledInputs;

  child(0) = child(0)->preCodeGen(generator, externalInputs, pulledNewInputs);
  if (! child(0).getPtr())
    return NULL;

  // process additional input value ids the child wants
  getGroupAttr()->addCharacteristicInputs(childPulledInputs);
  pulledNewInputs += childPulledInputs;

  // The VEG expressions in the selection predicates and the characteristic
  // outputs can reference any expression that is either a potential output
  // or a characteristic input for this RelExpr. Supply these values for
  // rewriting the VEG expressions.
  //
  ValueIdSet availableValues;
  getInputValuesFromParentAndChildren(availableValues);

  // The unPackExpr() and packingFactor() expressions have access 
  // to only the Input Values. These can come from the parent or be
  // the outputs of the child.
  //
  unPackExpr().
    replaceVEGExpressions(availableValues,
                          getGroupAttr()->getCharacteristicInputs());

  packingFactor().
    replaceVEGExpressions(availableValues,
                          getGroupAttr()->getCharacteristicInputs());

  if (rowwiseRowset())
    {
      if (rwrsInputSizeExpr())
	((ItemExpr*)rwrsInputSizeExpr())->
	  replaceVEGExpressions(availableValues,
				getGroupAttr()->getCharacteristicInputs());

      if (rwrsMaxInputRowlenExpr())
	((ItemExpr*)rwrsMaxInputRowlenExpr())->
	  replaceVEGExpressions(availableValues,
				getGroupAttr()->getCharacteristicInputs());

      if (rwrsBufferAddrExpr())
	((ItemExpr*)rwrsBufferAddrExpr())->
	  replaceVEGExpressions(availableValues,
				getGroupAttr()->getCharacteristicInputs());
    }

  // The selectionPred has access to only the output values generated by
  // UnPackRows and input values from the parent.
  //
  getInputAndPotentialOutputValues(availableValues);

  // Rewrite the selection predicates.
  //
  NABoolean replicatePredicates = TRUE;
  selectionPred().replaceVEGExpressions
    (availableValues,
     getGroupAttr()->getCharacteristicInputs(),
     FALSE, // no key predicates here
     0 /* no need for idempotence here */,
     replicatePredicates
     ); 

  // Replace VEG references in the outputs and remove redundant
  // outputs.
  //
  getGroupAttr()->resolveCharacteristicOutputs
    (availableValues,
     getGroupAttr()->getCharacteristicInputs());

  Lng32 memLimit = (Lng32)CmpCommon::getDefaultNumeric(MEMORY_LIMIT_ROWSET_IN_MB);
  if (memLimit > 0)
  {
    Lng32 rowLength = getGroupAttr()->getCharacteristicOutputs().getRowLength();
    Lng32 rowsetSize = getGroupAttr()->getOutputLogPropList()[0]->getResultCardinality().value() ;
    Lng32 memNeededMB = (rowLength * rowsetSize)/(1024 * 1024);
    if (memLimit < memNeededMB)
    {
      *CmpCommon::diags() << DgSqlCode(-30050) << DgInt0(memNeededMB) 
                          << DgInt1(memLimit) << DgInt2(rowLength) 
                          << DgInt3(rowsetSize);
      GenExit();
      return NULL;
    }
  }

  generator->oltOptInfo()->setMultipleRowsReturned(TRUE);

  markAsPreCodeGenned();

  return this;
} // PhysUnPackRows::preCodeGen


short
PhysUnPackRows::codeGen(Generator *generator) 
{
  // Get handles on expression generator, map table, and heap allocator
  //
  ExpGenerator *expGen = generator->getExpGenerator();
  Space *space = generator->getSpace();

  // Allocate a new map table for this operation
  //
  MapTable *localMapTable = generator->appendAtEnd();

  // Generate the child and capture the task definition block and a description
  // of the reply composite row layout and the explain information.
  //
  child(0)->codeGen(generator);

  ComTdb *childTdb = (ComTdb*)(generator->getGenObj());

  ex_cri_desc *childCriDesc = generator->getCriDesc(Generator::UP);

  ExplainTuple *childExplainTuple = generator->getExplainTuple();
  
  // Make all of my child's outputs map to ATP 1. Since they are
  // not needed above, they will not be in the work ATP (0).
  // (Later, they will be removed from the map table)
  //
  localMapTable->setAllAtp(1);

  // Generate the given and returned composite row descriptors.
  // unPackRows adds a tupp (for the generated outputs) to the
  // row given by the parent. The workAtp will have the 2 more 
  // tupps (1 for the generated outputs and another for the
  // indexValue) than the given.
  //
  ex_cri_desc *givenCriDesc = generator->getCriDesc(Generator::DOWN);

  ex_cri_desc *returnedCriDesc = 
#pragma nowarn(1506)   // warning elimination 
    new(space) ex_cri_desc(givenCriDesc->noTuples() + 1, space);
#pragma warn(1506)  // warning elimination 

  ex_cri_desc *workCriDesc = 
#pragma nowarn(1506)   // warning elimination 
    new(space) ex_cri_desc(givenCriDesc->noTuples() + 2, space);
#pragma warn(1506)  // warning elimination 


  // unPackCols is the next to the last Tp in Atp 0, the work ATP.
  // and the last Tp in the returned ATP.
  //
  const Int32 unPackColsAtpIndex = workCriDesc->noTuples() - 2;
  const Int32 unPackColsAtp = 0;

  // The length of the new tuple which will contain the columns
  // generated by unPackRows
  //
  ULng32 unPackColsTupleLen;

  // The Tuple Desc describing the tuple containing the new unPacked columns
  // It is generated when the expression is generated.
  //
  ExpTupleDesc *unPackColsTupleDesc = 0;

  // indexValue is the last Tp in Atp 0, the work ATP.
  //
  const Int32 indexValueAtpIndex = workCriDesc->noTuples() - 1;
  const Int32 indexValueAtp = 0;

  // The length of the work tuple which will contain the value
  // of the index.  This should always be sizeof(int).
  //
  ULng32 indexValueTupleLen = 0;

  // The Tuple Desc describing the tuple containing the new unPacked columns
  // It is generated when the expression is generated.
  //
  ExpTupleDesc *indexValueTupleDesc = 0;

  ValueIdList indexValueList;

  if (indexValue() != NULL_VALUE_ID)
    {
      indexValueList.insert(indexValue());
      
      expGen->processValIdList(indexValueList,
			       ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
			       indexValueTupleLen,
			       indexValueAtp,
			       indexValueAtpIndex,
			       &indexValueTupleDesc,
			       ExpTupleDesc::SHORT_FORMAT);
      
      GenAssert(indexValueTupleLen == sizeof(Int32),
		"UnPackRows::codeGen: Internal Error");
    }

  // If a packingFactor exists, generate a move expression for this.
  // It is assumed that the packingFactor expression evaluates to a
  // 4 byte integer.
  //
  ex_expr *packingFactorExpr = 0;
  ULng32 packingFactorTupleLen;
  
  if(packingFactor().entries() > 0) {
    expGen->generateContiguousMoveExpr(packingFactor(),
                                       -1, 
                                       unPackColsAtp,
                                       unPackColsAtpIndex,
                                       ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
                                       packingFactorTupleLen,
                                       &packingFactorExpr);

    GenAssert(packingFactorTupleLen == sizeof(Int32),
              "UnPackRows::codeGen: Internal Error");
  }

  // Generate the UnPack expressions.
  //
  // characteristicOutputs() - refers to the list of expressions
  // to be move to another tuple.
  //
  // 0 - Do not add conv. nodes.
  //
  // unPackColsAtp - this expression will move data to the
  // unPackColsAtp (0) ATP.
  //
  // unPackColsAtpIndex - within the unPackColsAtp (0) ATP, the destination
  // for this move expression will be the unPackColsAtpIndex TP. This should
  // be the next to the last TP of the work ATP. (The indexValue will be in
  // the last position)
  //
  // SQLARK_EXPLODED_FORMAT - generate the move expression to construct
  // the destination tuple in EXPLODED FORMAT.
  //
  // unPackColsTupleLen - This is an output which will contain the length
  // of the destination Tuple.
  //
  // &unPackColsExpr - The address of the pointer to the expression
  // which will be generated.
  //
  // &unPackColsTupleDesc - The address of the tuple descriptor which is
  // generated.  This describes the destination tuple of the move expression.
  //
  // SHORT_FORMAT - generate the unPackColsTupleDesc in the SHORT FORMAT.
  //
  ex_expr *unPackColsExpr = 0;

  expGen->
    genGuardedContigMoveExpr(selectionPred(),
			     getGroupAttr()->getCharacteristicOutputs(),
                             0, // No Convert Nodes added
                             unPackColsAtp,
                             unPackColsAtpIndex,
                             ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
                             unPackColsTupleLen,
                             &unPackColsExpr,
                             &unPackColsTupleDesc,
                             ExpTupleDesc::SHORT_FORMAT);

#pragma nowarn(1506)   // warning elimination 
  workCriDesc->setTupleDescriptor(unPackColsAtpIndex,
#pragma warn(1506)  // warning elimination 
                                  unPackColsTupleDesc);

#pragma nowarn(1506)   // warning elimination 
  returnedCriDesc->setTupleDescriptor(unPackColsAtpIndex,
#pragma warn(1506)  // warning elimination 
                                      unPackColsTupleDesc);


  // expressions for rowwise rowset implementation.
  ex_expr * rwrsInputSizeExpr = 0;
  ex_expr * rwrsMaxInputRowlenExpr = 0;
  ex_expr * rwrsBufferAddrExpr = 0;
  ULng32 rwrsInputSizeExprLen = 0;
  ULng32 rwrsMaxInputRowlenExprLen = 0;
  ULng32 rwrsBufferAddrExprLen = 0;

  const Int32 rwrsAtp = 1;
  const Int32 rwrsAtpIndex = workCriDesc->noTuples() - 2;
  ExpTupleDesc * rwrsTupleDesc = 0;
  ValueIdList rwrsVidList;
  if (rowwiseRowset())
    {
      rwrsVidList.insert(this->rwrsInputSizeExpr()->getValueId());
      expGen->generateContiguousMoveExpr(rwrsVidList, 
					 0 /*don't add conv nodes*/,
					 rwrsAtp, rwrsAtpIndex,
					 ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
					 rwrsInputSizeExprLen, 
					 &rwrsInputSizeExpr,
					 &rwrsTupleDesc,ExpTupleDesc::SHORT_FORMAT);
      
      rwrsVidList.clear();
      rwrsVidList.insert(this->rwrsMaxInputRowlenExpr()->getValueId());
      expGen->generateContiguousMoveExpr(rwrsVidList, 
					 0 /*don't add conv nodes*/,
					 rwrsAtp, rwrsAtpIndex,
					 ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
					 rwrsMaxInputRowlenExprLen, 
					 &rwrsMaxInputRowlenExpr,
					 &rwrsTupleDesc,ExpTupleDesc::SHORT_FORMAT);

      rwrsVidList.clear();
      rwrsVidList.insert(this->rwrsBufferAddrExpr()->getValueId());
      expGen->generateContiguousMoveExpr(rwrsVidList, 
					 0 /*don't add conv nodes*/,
					 rwrsAtp, rwrsAtpIndex,
					 ExpTupleDesc::SQLARK_EXPLODED_FORMAT,
					 rwrsBufferAddrExprLen, 
					 &rwrsBufferAddrExpr,
					 &rwrsTupleDesc,ExpTupleDesc::SHORT_FORMAT);

      expGen->assignAtpAndAtpIndex(rwrsOutputVids(), 
				   unPackColsAtp, unPackColsAtpIndex);
    }

  // Move the generated maptable entries, to the localMapTable,
  // so that all other entries can later be removed.
  //
  for(ValueId outputValId = getGroupAttr()->getCharacteristicOutputs().init();
      getGroupAttr()->getCharacteristicOutputs().next(outputValId);
      getGroupAttr()->getCharacteristicOutputs().advance(outputValId)) {
    
    generator->addMapInfoToThis(localMapTable, outputValId,
				generator->getMapInfo(outputValId)->
				getAttr());

    // Indicate that code was generated for this map table entry.
    //
    generator->getMapInfoFromThis(localMapTable, outputValId)->codeGenerated();
  }

  NABoolean tolerateNonFatalError = FALSE;

  if (isRowsetIterator() && (generator->getTolerateNonFatalError())) {
    tolerateNonFatalError = TRUE;
    setTolerateNonFatalError(RelExpr::NOT_ATOMIC_);
  }


  // Allocate the UnPack TDB
  //
  ComTdbUnPackRows *unPackTdb = NULL;

  if (rowwiseRowset())
    {
      unPackTdb =
	new (space) ComTdbUnPackRows(NULL, //childTdb,
				     rwrsInputSizeExpr,
				     rwrsMaxInputRowlenExpr,
				     rwrsBufferAddrExpr,
				     rwrsAtpIndex,
				     givenCriDesc,
				     returnedCriDesc,
				     workCriDesc,
				     16,
				     1024,
				     (Cardinality) getGroupAttr()->
				     getOutputLogPropList()[0]->
				     getResultCardinality().value(),
				     2, 20000);
    }
  else
    {

      // Base the initial queue size on the est. cardinality.
      // UnPackRows does not do dyn queue resize, so passed in
      // queue size values represent initial (and final) queue
      // sizes (not max queue sizes).
      //
      ULng32 rowsetSize = 
          getGroupAttr()->getOutputLogPropList()[0]->getResultCardinality().value();
      double  memoryLimitPerInstance =
              ActiveSchemaDB()->getDefaults().getAsLong(EXE_MEMORY_FOR_UNPACK_ROWS_IN_MB) * 1024 * 1024;

      rowsetSize = (rowsetSize < 1024 ? 1024 : rowsetSize);
      double estimatedMemory = rowsetSize * unPackColsTupleLen;
 
      if (estimatedMemory > memoryLimitPerInstance)
      {
         estimatedMemory = memoryLimitPerInstance;
         rowsetSize = estimatedMemory / unPackColsTupleLen;
         rowsetSize = MAXOF(rowsetSize, 1);
      }

      queue_index upQueueSize = ex_queue::roundUp2Power((queue_index)rowsetSize);

      unPackTdb =
	new (space) ComTdbUnPackRows(childTdb,
				     packingFactorExpr,
				     unPackColsExpr,
#pragma nowarn(1506)   // warning elimination 
				     unPackColsTupleLen,
				     unPackColsAtpIndex,
				     indexValueAtpIndex,
				     givenCriDesc,
				     returnedCriDesc,
				     workCriDesc,
				     16,
				     upQueueSize,
				     (Cardinality) getGroupAttr()->
				     getOutputLogPropList()[0]->
				     getResultCardinality().value(),
				     isRowsetIterator(),
				     tolerateNonFatalError);
    }

#pragma warn(1506)  // warning elimination 
  generator->initTdbFields(unPackTdb);

  // Remove child's outputs from mapTable, They are not needed
  // above.
  //
  generator->removeAll(localMapTable);

  // Add the explain Information for this node to the EXPLAIN
  // Fragment.  Set the explainTuple pointer in the generator so
  // the parent of this node can get a handle on this explainTuple.
  //
  if(!generator->explainDisabled()) {
    generator->setExplainTuple(addExplainInfo(unPackTdb,
                                              childExplainTuple,
                                              0,
                                              generator));
  }     
        
  // Restore the Cri Desc's and set the return object.
  //
  generator->setCriDesc(givenCriDesc, Generator::DOWN);
  generator->setCriDesc(returnedCriDesc, Generator::UP);
  generator->setGenObj(this, unPackTdb);

  
  return 0;
}
