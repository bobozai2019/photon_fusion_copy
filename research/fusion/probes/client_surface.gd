extends SceneTree

func own_names(entries: Array) -> Array:
    var names := []
    for entry in entries:
        names.append(str(entry.name))
    names.sort()
    return names

func type_name(info: Dictionary) -> String:
    if int(info.type) == TYPE_NIL:
        return "Variant" if (int(info.usage) & PROPERTY_USAGE_NIL_IS_VARIANT) != 0 else "void"
    if not str(info.class_name).is_empty():
        return str(info.class_name)
    return type_string(int(info.type))

func callable_signatures(entries: Array) -> Dictionary:
    var signatures := {}
    for entry in entries:
        var args := PackedStringArray()
        for arg in entry.args:
            args.append("%s %s" % [type_name(arg), arg.name])
        signatures[str(entry.name)] = "(%s) -> %s [defaults=%d flags=%d]" % [
            ", ".join(args), type_name(entry["return"]), entry.default_args.size(), entry.flags]
    return signatures

func _initialize() -> void:
    var result := {}
    for cls in ClassDB.get_class_list():
        if not str(cls).begins_with("Fusion"):
            continue
        var enums := {}
        for enumeration in ClassDB.class_get_enum_list(cls, true):
            var values := {}
            for constant_name in ClassDB.class_get_enum_constants(cls, enumeration, true):
                values[constant_name] = ClassDB.class_get_integer_constant(cls, constant_name)
            enums[enumeration] = values
        var methods := ClassDB.class_get_method_list(cls, true)
        var signals := ClassDB.class_get_signal_list(cls, true)
        var properties := ClassDB.class_get_property_list(cls, true)
        var descriptors := {}
        for prop in properties:
            descriptors[str(prop.name)] = "%s [usage=%d hint=%d %s]" % [
                type_name(prop), prop.usage, prop.hint, prop.hint_string]
        result[cls] = {
            "parent": ClassDB.get_parent_class(cls),
            "methods": own_names(methods),
            "properties": own_names(properties),
            "signals": own_names(signals),
            "enums": enums,
            "method_signatures": callable_signatures(methods),
            "signal_signatures": callable_signatures(signals),
            "property_descriptors": descriptors,
        }
    print("FUSION_CLASSDB ", JSON.stringify(result))
    quit(int(result.is_empty()))

