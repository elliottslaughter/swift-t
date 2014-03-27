package exm.stc.ic.opt;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.ListIterator;
import java.util.Set;

import org.apache.log4j.Logger;

import exm.stc.common.Settings;
import exm.stc.common.exceptions.UserException;
import exm.stc.common.lang.Arg;
import exm.stc.common.lang.Types;
import exm.stc.common.lang.Types.StructType;
import exm.stc.common.lang.Types.StructType.StructField;
import exm.stc.common.lang.Var;
import exm.stc.common.util.MultiMap;
import exm.stc.common.util.StackLite;
import exm.stc.ic.opt.OptimizerPass.FunctionOptimizerPass;
import exm.stc.ic.tree.ICContinuations.Continuation;
import exm.stc.ic.tree.ICInstructions.Instruction;
import exm.stc.ic.tree.ICTree.Block;
import exm.stc.ic.tree.ICTree.Function;
import exm.stc.ic.tree.ICTree.Statement;
import exm.stc.ic.tree.ICTree.StatementType;
import exm.stc.ic.tree.Opcode;
import exm.stc.ic.tree.TurbineOp;

/**
 * TODO: combine with array build?
 */
public class StructBuild extends FunctionOptimizerPass {

  @Override
  public String getPassName() {
    return "Struct build";
  }

  @Override
  public String getConfigEnabledKey() {
    return Settings.OPT_ARRAY_BUILD;
  }

  @Override
  public void optimize(Logger logger, Function f) throws UserException {
    structBuildRec(logger, f.mainBlock());
  }

  private void structBuildRec(Logger logger, Block block) {
    // Track all assigned struct paths
    MultiMap<Var, List<String>> assignedPaths = new MultiMap<Var, List<String>>();
    
    // Find all struct assign statements in block
    for (Statement stmt: block.getStatements()) {
      if (stmt.type() == StatementType.INSTRUCTION) {
        Instruction inst = stmt.instruction();
        if (inst.op == Opcode.STRUCT_STORE_SUB) {
          Var struct = inst.getOutput(0);
          List<Arg> inputs = inst.getInputs();
          List<Arg> fields = inputs.subList(1, inputs.size());
          assignedPaths.put(struct, Arg.extractStrings(fields));
        }
      }
    }
    
    // Check if all fields were assigned
    for (Var candidate: assignedPaths.keySet()) {
      Set<List<String>> expectedPaths = allAssignablePaths(candidate);
      List<List<String>> assigned = assignedPaths.get(candidate);
      
      for (List<String> path: assigned) {
        boolean found = expectedPaths.remove(path);
        if (!found) {
          logger.warn("Invalid or double-assigned struct field: " +
                       candidate.name() + "." + path);
        }
      }
      if (expectedPaths.isEmpty()) {
        doStructBuildTransform(logger, block, candidate, assigned.size());
      }
    }
    
    for (Continuation cont: block.allComplexStatements()) {
      for (Block cb: cont.getBlocks()) {
        structBuildRec(logger, cb);
      }
    }
  }

  /**
   * Work out all of the paths that need to be assigned for the struct
   * @param candidate
   * @return
   */
  private Set<List<String>> allAssignablePaths(Var candidate) {
    Set<List<String>> paths = new HashSet<List<String>>();
    StructType type = (StructType)candidate.type().getImplType();
    addAssignablePaths(type, new StackLite<String>(), paths);
    return paths;
  }

  private void addAssignablePaths(StructType type, StackLite<String> prefix,
      Set<List<String>> paths) {
    for (StructField f: type.getFields()) {
      prefix.push(f.getName());
      if (Types.isRef(f.getType())) {
        // Don't assign
      } else if (Types.isStruct(f.getType())) {
        addAssignablePaths((StructType)f.getType().getImplType(), prefix,
                           paths);
      } else {
        // Must be assigned
        paths.add(new ArrayList<String>(prefix));
      }
      prefix.pop();
    }
  }

  /**
   * Replace struct stores with struct build
   * @param logger
   * @param block
   * @param candidate
   * @param fieldsToAssign number of fields assigned
   */
  private void
      doStructBuildTransform(Logger logger, Block block, Var candidate,
                             int fieldsToAssign) {
    int fieldsAssigned = 0;
    List<List<String>> fieldPaths = new ArrayList<List<String>>();
    List<Arg> fieldVals = new ArrayList<Arg>();
    
    ListIterator<Statement> stmtIt = block.statementIterator();
    while (stmtIt.hasNext()) {
      Statement stmt = stmtIt.next();
      if (stmt.type() == StatementType.INSTRUCTION) {
        Instruction inst = stmt.instruction();
        if (inst.op == Opcode.STRUCT_STORE_SUB) {
          Var struct = inst.getOutput(0);
          if (struct.equals(candidate)) {
            stmtIt.remove();
            fieldsAssigned++;
            
            List<Arg> inputs = inst.getInputs();
            fieldPaths.add(
                Arg.extractStrings(inputs.subList(1, inputs.size())));
            fieldVals.add(inputs.get(0));
          }
        }
      }
      if (fieldsAssigned == fieldsToAssign) {
        // Assign to local struct then store
        Var localStruct = OptUtil.createDerefTmp(block, candidate);
        stmtIt.add(TurbineOp.structLocalBuild(localStruct, fieldPaths, fieldVals));
        stmtIt.add(TurbineOp.assignStruct(candidate, localStruct.asArg()));
        return;
      }
    }
    
    // Should not fall out of loop
    assert(false);
  }

}
