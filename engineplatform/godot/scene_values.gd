@tool
extends RefCounted

# Collections carry only built-in value types. Object references use the separate
# object_ref field. No remote input can select a resource or script to instantiate.
const LEAVES := [TYPE_NIL,TYPE_BOOL,TYPE_INT,TYPE_FLOAT,TYPE_STRING,TYPE_VECTOR2,TYPE_VECTOR3,
    TYPE_VECTOR4,TYPE_QUATERNION,TYPE_COLOR,TYPE_PACKED_BYTE_ARRAY,TYPE_PACKED_INT32_ARRAY,
    TYPE_PACKED_FLOAT32_ARRAY,TYPE_PACKED_VECTOR2_ARRAY]
const MAX_DEPTH := 12
var data:PackedByteArray
var cursor:=0
var valid:=true
var limit:=0

static func encode_integer(value: int) -> PackedByteArray:
    var bytes := PackedByteArray()
    bytes.resize(8)
    bytes.encode_s64(0, value)
    return bytes

func encode(value:Variant,kind:String,capacity:int) -> Variant:
    if kind=="integer64":
        if typeof(value)!=TYPE_INT: return null
        return encode_integer(value)
    if typeof(value)!=(TYPE_ARRAY if kind=="array_value" else TYPE_DICTIONARY): return null
    limit=capacity
    _put(value,0)
    return data if valid else null

func decode(bytes:PackedByteArray,kind:String) -> Variant:
    if kind=="integer64": return bytes.decode_s64(0) if bytes.size()==8 else null
    data=bytes
    var result=_take(0)
    if typeof(result)!=(TYPE_ARRAY if kind=="array_value" else TYPE_DICTIONARY): return null
    return result if valid and cursor==data.size() else null

func _plain(type:int) -> bool:
    return type in LEAVES or type in [TYPE_ARRAY,TYPE_DICTIONARY]

func _put(value:Variant,depth:int) -> void:
    if not valid: return
    var type:=typeof(value)
    if depth>MAX_DEPTH or data.size()>=limit or not _plain(type):
        valid=false
        return
    data.append(type)
    if type==TYPE_ARRAY or type==TYPE_DICTIONARY:
        if value.size()>mini(65535,limit): valid=false; return
        var types:Array
        if type==TYPE_ARRAY: types=[value.get_typed_builtin()]
        else: types=[value.get_typed_key_builtin(),value.get_typed_value_builtin()]
        for t:int in types:
            if not _plain(t): valid=false; return
            data.append(t)
        data.append(value.size() & 255)
        data.append(value.size() >> 8)
        for item in value:
            _put(item,depth+1)
            if type==TYPE_DICTIONARY: _put(value[item],depth+1)
            if not valid: return
    else:
        var bytes:=var_to_bytes(value)
        if bytes.size()>limit-data.size(): valid=false; return
        data.append_array(bytes)
    valid=valid and data.size()<=limit

func _take(depth:int) -> Variant:
    if not valid or cursor>=data.size() or depth>MAX_DEPTH: valid=false; return null
    var type:=int(data[cursor])
    cursor+=1
    if type in LEAVES:
        if data.size()-cursor<4 or data.decode_u16(cursor)!=type: valid=false; return null
        var size:=data.decode_var_size(cursor,false)
        if size<4 or size>data.size()-cursor: valid=false; return null
        var result=data.decode_var(cursor,false)
        cursor+=size
        return result
    if not type in [TYPE_ARRAY,TYPE_DICTIONARY]: valid=false; return null
    var dictionary:=type==TYPE_DICTIONARY
    if data.size()-cursor<(4 if dictionary else 3): valid=false; return null
    var first:=int(data[cursor])
    cursor+=1
    var second:=0
    if dictionary:
        second=int(data[cursor])
        cursor+=1
    if not _plain(first) or not _plain(second): valid=false; return null
    var count:=data.decode_u16(cursor)
    cursor+=2
    if count>data.size()-cursor: valid=false; return null
    var result:Variant=Dictionary({},first,&"",null,second,&"",null) if dictionary else Array([],first,&"",null)
    for i in count:
        var item=_take(depth+1)
        if not valid or (first and typeof(item)!=first): valid=false; return null
        if dictionary:
            var value=_take(depth+1)
            if not valid or (second and typeof(value)!=second) or result.has(item): valid=false; return null
            result[item]=value
        else: result.append(item)
    return result
