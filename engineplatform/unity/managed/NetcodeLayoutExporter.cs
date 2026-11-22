using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using UnityEngine;

namespace Netcode {
public sealed class NetcodeLayoutExporter : MonoBehaviour {
    [Serializable]
    public class Binding {
        public int Id = 1;
        public Component Target;
        public string Member;
        public int Limit;
    }
    public int Schema = 1, Version = 1;
    public string SchemaName = "Actor";
    public Binding[] Fields = Array.Empty<Binding>();
    static readonly Dictionary<Type, string> Kinds = new Dictionary<Type, string> {
        { typeof(float), "float" },   { typeof(bool), "flag" },       { typeof(int), "int" },
        { typeof(Vector2), "vec2" },  { typeof(Vector3), "vec3" },    { typeof(Quaternion), "quat" },
        { typeof(string), "string" }, { typeof(int[]), "int_array" }, { typeof(float[]), "float_array" },
        { typeof(Vector4),"vec4" }, {typeof(Color),"color"}, {typeof(ulong),"object_ref"}, {typeof(byte[]),"bytes"},
        { typeof(Vector2[]),"vec2_array" }, {typeof(Dictionary<int,int>),"int_map"}, {typeof(LinkedList<int>),"int_list"}
    };
    static bool ValidToken(string name) {
        return !string.IsNullOrEmpty(name) &&
               !Array.Exists(name.ToCharArray(), c => char.IsWhiteSpace(c) || c == '#');
    }
    public string BuildText() {
        if (!ValidToken(SchemaName))
            throw new ArgumentException("Invalid schema name");
        var definitions = new List<Binding>(Fields);
        definitions.Sort((a, b) => a.Id.CompareTo(b.Id));
        var text = new StringBuilder().Append($"schema {Schema} {Version} {SchemaName}\n");
        foreach (var field in definitions) {
            if (!field.Target || !field.Target.transform.IsChildOf(transform) || !ValidToken(field.Member))
                throw new ArgumentException("Invalid replicated member");
            var type = field.Target.GetType();
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.Public;
            var memberType = type.GetField(field.Member, flags)?.FieldType ??
                             type.GetProperty(field.Member, flags)?.PropertyType;
            if (memberType == null || !Kinds.TryGetValue(memberType, out var kind))
                throw new ArgumentException("Unsupported field " + field.Member);
            var names = new Stack<string>();
            for (var node = field.Target.transform; node != transform; node = node.parent) {
                if (!ValidToken(node.name) || node.name.Contains("/"))
                    throw new ArgumentException("Invalid child name");
                names.Push(node.name);
            }
            var path = names.Count == 0 ? "." : string.Join("/", names);
            text.Append($"{kind} {field.Id} {path} {field.Member}");
            if (memberType == typeof(string) || memberType.IsArray || kind=="int_map" || kind=="int_list")
                text.Append($" {field.Limit}");
            text.Append('\n');
        }
        if (!NetcodeGame.ValidateLayout(text.ToString()))
            throw new ArgumentException("Invalid field IDs, limits or schema size");
        return text.ToString();
    }
#if UNITY_EDITOR
    [ContextMenu("Export replication layout")]
    void ExportLayout() {
        var content = BuildText();
        var path = UnityEditor.EditorUtility.SaveFilePanel("Export replication layout", Application.dataPath,
                                                           SchemaName, "cfg");
        if (!string.IsNullOrEmpty(path))
            System.IO.File.WriteAllText(path, content, new UTF8Encoding(false));
    }
#endif
}
}
