@tool
extends RefCounted

const KINDS := {TYPE_FLOAT:"float", TYPE_BOOL:"flag", TYPE_INT:"int", TYPE_VECTOR2:"vec2",
    TYPE_VECTOR3:"vec3", TYPE_QUATERNION:"quat", TYPE_STRING:"string", TYPE_VECTOR4:"vec4",
    TYPE_COLOR:"color", TYPE_PACKED_BYTE_ARRAY:"bytes", TYPE_PACKED_VECTOR2_ARRAY:"vec2_array",
    TYPE_DICTIONARY:"int_map", TYPE_ARRAY:"int_list", TYPE_PACKED_INT32_ARRAY:"int_array",
    TYPE_PACKED_FLOAT32_ARRAY:"float_array"}

const WIDTH := {"float":4,"flag":1,"int":4,"vec2":8,"vec3":12,"quat":16,"vec4":16,"color":16,"object_ref":8}
const ELEMENT := {"string":1,"bytes":1,"int_array":4,"float_array":4,"vec2_array":8,"int_map":8,"int_list":4}

static func kind(info: Dictionary) -> String:
    var type: int = info.type
    if type == TYPE_OBJECT:
        var cls := String(info.get("class_name",""))
        if cls.is_empty(): cls = String(info.get("hint_string",""))
        if cls=="Node" or cls=="Object" or ClassDB.is_parent_class(cls,"Node"): return "object_ref"
        for entry in ProjectSettings.get_global_class_list():
            if String(entry.class)==cls:
                var script:Script=load(entry.path)
                if script and ClassDB.is_parent_class(script.get_instance_base_type(),"Node"): return "object_ref"
        return ""
    if type == TYPE_ARRAY: return "array_value"
    if type == TYPE_DICTIONARY: return "dictionary_value"
    if type == TYPE_INT: return "integer64"
    return KINDS.get(type,"")

static func wire(kind_name:String) -> String:
    if kind_name in ["array_value","dictionary_value","integer64"]: return "bytes"
    return kind_name

static func wire_id(key: String) -> int:
    return 1 + key.sha256_text().substr(0,8).hex_to_int() % 65535

static func matches_node(node:Node,cls:String) -> bool:
    if cls.is_empty() or node.is_class(cls): return true
    var script:Script=node.get_script()
    while script:
        if script.get_global_name()==cls: return true
        script=script.get_base_script()
    return false

# Reflection returns either type names or numeric Godot property hints depending on
# whether the declaration came from native metadata or a GDScript signature.
static func _builtin(hint:String) -> int:
    if hint.is_empty() or hint=="Variant": return TYPE_NIL
    var prefix:=hint.get_slice(":",0)
    if prefix.is_valid_int(): return int(prefix)
    for i in TYPE_MAX:
        if type_string(i)==hint: return i
    return -1

static func collection(value:Variant,hint:String) -> Variant:
    if hint.is_empty(): return value
    var types:=hint.split(";",true)
    var first:=_builtin(types[0])
    if first<0 or first==TYPE_OBJECT: return null
    if value is Array and types.size()==1:
        if value.get_typed_builtin()==first: return value
        if first:
            for item in value:
                if typeof(item)!=first: return null
        return Array(value,first,&"",null)
    if value is Dictionary and types.size()==2:
        var second:=_builtin(types[1])
        if second<0 or second==TYPE_OBJECT: return null
        if value.get_typed_key_builtin()==first and value.get_typed_value_builtin()==second: return value
        for key in value:
            if (first and typeof(key)!=first) or (second and typeof(value[key])!=second): return null
        return Dictionary(value,first,&"",null,second,&"",null)
    return null
