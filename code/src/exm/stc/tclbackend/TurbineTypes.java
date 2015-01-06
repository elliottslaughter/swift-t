package exm.stc.tclbackend;

import java.util.ArrayList;
import java.util.List;

import exm.stc.common.exceptions.STCRuntimeError;
import exm.stc.common.lang.Types;
import exm.stc.common.lang.Types.NestedContainerInfo;
import exm.stc.common.lang.Types.PrimType;
import exm.stc.common.lang.Types.RefType;
import exm.stc.common.lang.Types.StructType;
import exm.stc.common.lang.Types.StructType.StructField;
import exm.stc.common.lang.Types.Type;
import exm.stc.common.lang.Types.Typed;
import exm.stc.common.util.Pair;
import exm.stc.common.util.StackLite;
import exm.stc.tclbackend.Turbine.TypeName;
import exm.stc.tclbackend.tree.Expression;
import exm.stc.tclbackend.tree.LiteralInt;
import exm.stc.tclbackend.tree.TclList;
import exm.stc.tclbackend.tree.TclString;
import exm.stc.tclbackend.tree.Token;


/**
 * Helper methods that encode type information in ways that is usable by Turbine
 */
public class TurbineTypes {

  /**
   * Return the full type required to create data by ADLB.
   * In case of simple data, just the name - e.g. "int", or "mystruct"
   * For containers, may need to have key/value/etc as separate arguments
   * @param type
   * @param createArgs
   * @return
   */
  public static List<Expression> dataDeclarationFullType(Type type) {
    List<Expression> typeExprList = new ArrayList<Expression>();
    // Basic data type
    typeExprList.add(reprType(type));
    // Subscript and value type for containers only
    if (Types.isArray(type)) {
      typeExprList.add(arrayKeyType(type, true)); // key
      typeExprList.add(arrayValueType(type, true)); // value
    } else if (Types.isBag(type)) {
      typeExprList.add(bagValueType(type, true));
    }
    return typeExprList;
  }

  public static TypeName adlbPrimType(PrimType pt) {
    switch (pt) {
      case INT:
      case BOOL:
      case VOID:
        return Turbine.ADLB_INT_TYPE;
      case BLOB:
        return Turbine.ADLB_BLOB_TYPE;
      case FLOAT:
        return Turbine.ADLB_FLOAT_TYPE;
      case STRING:
        return Turbine.ADLB_STRING_TYPE;
      default:
        throw new STCRuntimeError("Unknown ADLB representation for " + pt);
    }
  }

  public static TypeName structTypeName(Type type) {
    assert(Types.isStruct(type) || Types.isStructLocal(type));
    // Prefix Swift name with prefix to indicate struct type
  
    StructType st = (StructType)type.getImplType();
    return new TypeName("s:" + st.getStructTypeName());
  }

  /**
   * @param t
   * @return ADLB representation type
   */
  public static TypeName reprType(Type t) {
    if (Types.isScalarFuture(t) || Types.isScalarUpdateable(t)) {
      return adlbPrimType(t.primType());
    } else if (Types.isRef(t)) {
      return refReprType(t.memberType());
    } else if (Types.isArray(t)) {
      return Turbine.ADLB_CONTAINER_TYPE;
    } else if (Types.isBag(t)) {
      return Turbine.ADLB_MULTISET_TYPE;
    } else if (Types.isStruct(t)) {
      return structTypeName(t);
    } else if (Types.isFile(t)) {
      return Turbine.ADLB_FILE_TYPE;
    } else {
      throw new STCRuntimeError("Unknown ADLB representation type for " + t);
    }
  }

  public static TypeName refReprType(Type memberType) {
      if (Types.isFile(memberType)) {
        return Turbine.ADLB_FILE_REF_TYPE;
      } else {
        return Turbine.ADLB_REF_TYPE;
      }
  }

  /**
   * Representation type for value if stored into data store
   * @param t
   * @param creation
   * @return
   */
  public static TypeName valReprType(Type t) {
    if (Types.isScalarValue(t)) {
      return adlbPrimType(t.primType());
    } else if (Types.isArrayLocal(t)) {
      return Turbine.ADLB_CONTAINER_TYPE;
    } else if (Types.isBagLocal(t)) {
      return Turbine.ADLB_MULTISET_TYPE;
    } else if (Types.isFile(t)) {
      return Turbine.ADLB_FILE_REF_TYPE;
    } else if (Types.isScalarFuture(t) || Types.isContainer(t) ||
               Types.isRef(t)) {
      // Local handle to remote data
      return Turbine.ADLB_REF_TYPE;
    } else if (Types.isStructLocal(t)) {
      return structTypeName(t);
    } else {
      throw new STCRuntimeError("Unknown ADLB representation type for " + t);
    }
  }

  private static TypeName reprTypeHelper(boolean valueType, Type type) {
    TypeName reprType;
    if (valueType) {
      reprType = valReprType(type);
    } else {
      reprType = reprType(type);
    }
    return reprType;
  }

  public static TypeName arrayKeyType(Typed arr, boolean creation) {
    return reprType(Types.arrayKeyType(arr));
  }

  public static TypeName arrayValueType(Typed arrType, boolean creation) {
    return reprType(Types.containerElemType(arrType));
  }

  public static TypeName bagValueType(Typed bagType, boolean creation) {
    return reprType(Types.containerElemType(bagType));
  }

  /**
   * @param depths
   * @param i
   * @param type
   * @return (nesting depth, base type name)
   */
  public static Pair<Integer, Expression> recursiveTypeDescriptor(Type type) {
    Type baseType;
    int depth;
    if (Types.isContainer(type)) {
      NestedContainerInfo ai = new NestedContainerInfo(type);
      depth = ai.nesting;
      baseType = ai.baseType;
    } else if (Types.isStruct(type)) {
      depth = 0;
      baseType = new RefType(type, false);
    } else if (Types.isFuture((type)) || Types.isStruct(type)) {
      depth = 0;
      // Indicate that it's a future not a value
      // TODO: does mutability matter?
      baseType = new RefType(type, false);
    } else if (Types.isPrimValue(type) || Types.isStructLocal(type)) {
      depth = 0;
      baseType = type;
    } else {
      throw new STCRuntimeError("Not sure how to deep wait on type "
                                + type);
    }

    Expression baseTypeExpr;
    if (Types.isStruct(baseType) || Types.isStructRef(baseType)) {
      baseTypeExpr = structTypeDescriptorExpr(baseType);
    } else {
      baseTypeExpr = reprType(baseType);
    }

    return Pair.create(depth, baseTypeExpr);
  }

  /**
   * Return descriptor describing struct fields that are references to
   * other data.
   * Descriptor is list with following elements:
   *    ("struct"|"struct_ref"), fields, nest_levels, base_types
   *    field_paths: list of field path lists with recursive references
   *    nest_levels: list of nest levels for fields
   *    base_types: list of base types for fields
   *
   * @param type - struct or ref to struct
   * @return
   */
  private static Expression structTypeDescriptorExpr(Type type) {
    assert(Types.isStruct(type) || Types.isStructRef(type));

    StructType structType;
    boolean reference = Types.isRef(type);
    if (reference) {
      structType = (StructType)type.memberType();
    } else {
      structType = (StructType)type;
    }

    Token prefix = new Token(reference ? "struct_ref" : "struct");

    List<Expression> fieldPaths = new ArrayList<Expression>();
    List<LiteralInt> nestLevels = new ArrayList<LiteralInt>();
    List<Expression> baseTypes = new ArrayList<Expression>();

    StackLite<Expression> structPath = new StackLite<Expression>();
    buildStructTypeDescriptor(structType, structPath, fieldPaths,
                              nestLevels, baseTypes);

    return new TclList(prefix, new TclList(fieldPaths),
                       new TclList(nestLevels),
                       new TclList(baseTypes));
  }

  private static void buildStructTypeDescriptor(StructType structType,
      StackLite<Expression> structPath, List<Expression> fieldPaths,
      List<LiteralInt> nestLevels, List<Expression> baseTypes) {
    for (StructField f: structType.getFields()) {
      Type fieldType = f.getType();
      structPath.push(new TclString(f.getName(), true));
      if (Types.isRef(fieldType)) {
        fieldPaths.add(new TclList(structPath));

        Type derefed = fieldType.memberType();
        Pair<Integer, Expression> fieldTypeDescriptor =
            recursiveTypeDescriptor(derefed);
        nestLevels.add(new LiteralInt(fieldTypeDescriptor.val1));
        baseTypes.add(fieldTypeDescriptor.val2);
      } else if (Types.isStruct(fieldType)) {
        buildStructTypeDescriptor((StructType)fieldType, structPath,
                                  fieldPaths, nestLevels, baseTypes);
      } else {
        // Value - don't need to follow
      }
      structPath.pop();
    }
  }

  /**
   *
   * @param type
   * @param valueType
   * @param includeKeyTypes
   * @param includeBaseType
   * @param followRefs if false, stop at first reference type
   * @param includeRefs Include ref types followed in output
   * @return
   */
  public static List<TypeName> recursiveTypeList(Type type,
        boolean valueType, boolean includeKeyTypes,
        boolean includeBaseType, boolean followRefs,
        boolean includeRefs) {
    List<TypeName> typeList = new ArrayList<TypeName>();
    Type curr = type;
    while ((Types.isContainer(curr) || Types.isContainerLocal(curr))) {
      typeList.add(reprTypeHelper(valueType, curr));
      if (includeKeyTypes &&
          (Types.isArray(curr) || Types.isArrayLocal(curr))) {
        // Include key type if requested
        typeList.add(reprType(Types.arrayKeyType(curr)));
      }

      curr = Types.containerElemType(curr);
      if (followRefs && Types.isContainerRef(curr)) {
        // Strip off reference
        curr = Types.retrievedType(curr);
        if (includeRefs) {
          typeList.add(refReprType(curr));
        }
      }
    }

    while (followRefs && Types.isRef(curr)) {
      curr = Types.retrievedType(curr);
      if (includeRefs && includeBaseType) {
        typeList.add(refReprType(curr));
      }
    }

    if (followRefs &&
        (Types.isStruct(curr) || Types.isStructLocal(curr)) &&
        ((StructType)curr.getImplType()).hasRefField()) {
      StructType st = (StructType)curr.getImplType();
        // Need to follow refs
      typeList.add(recursiveStructType(st));
    } else if (includeBaseType) {
      // TODO: include type info
      typeList.add(reprTypeHelper(valueType, curr));
    }

    return typeList;
  }

  private static TypeName recursiveStructType(StructType st) {
    // TODO Auto-generated method stub
    return null;
  }

}
