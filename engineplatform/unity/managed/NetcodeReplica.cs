using System;
using System.Reflection;
using UnityEngine;

namespace Netcode {
// Attach to a native rigid body, or assign named members in field-ID order.
public sealed class NetcodeReplica : MonoBehaviour {
    public NetcodeLayoutExporter.Binding[] Fields = Array.Empty<NetcodeLayoutExporter.Binding>();
    public float TeleportDistance = 100, TeleportAngleDegrees = 15;
    public int TeleportGeneration, Phase;
    public bool Initialized { get; private set; }
    NetcodeGame game;
    uint id;
    Rigidbody body3;
    Rigidbody2D body2;
    readonly ForecastBody3D forecast3 = new ForecastBody3D();
    readonly ForecastBody forecast2 = new ForecastBody();
    FieldInfo[] members;
    PropertyInfo[] properties;
    NetcodeLayoutExporter.Binding[] bindings;
    public bool Bind(NetcodeGame session, uint objectId) {
        Unbind();
        if (session == null || objectId == 0)
            return false;
        bindings = (NetcodeLayoutExporter.Binding[])Fields.Clone();
        Array.Sort(bindings, (a, b) => a.Id.CompareTo(b.Id));
        body3 = null;
        body2 = null;
        if (bindings.Length == 0) {
            body3 = GetComponent<Rigidbody>();
            body2 = GetComponent<Rigidbody2D>();
            if (!body3 && !body2)
                return false;
        } else {
            members = new FieldInfo[bindings.Length];
            properties = new PropertyInfo[bindings.Length];
            for (int i = 0; i < bindings.Length; i++) {
                var b = bindings[i];
                if (!b.Target || !b.Target.transform.IsChildOf(transform) || b.Id <= 0 ||
                    (i > 0 && bindings[i - 1].Id == b.Id))
                    return false;
                var type = b.Target.GetType();
                const BindingFlags flags = BindingFlags.Public | BindingFlags.Instance;
                members[i] = type.GetField(b.Member, flags);
                properties[i] = type.GetProperty(b.Member, flags);
                if (members[i] != null
                        ? members[i].IsInitOnly
                        : properties[i]?.GetGetMethod() == null || properties[i].GetSetMethod() == null ||
                              properties[i].GetIndexParameters().Length != 0)
                    return false;
            }
        }
        if (!session.AttachReplica(objectId, this))
            return false;
        game = session;
        id = objectId;
        return true;
    }
    public ulong Reference => game?.FieldReference(id) ?? 0;
    public void Unbind() {
        game?.DetachReplica(id, this);
        game = null;
        id = 0;
        Initialized = false;
    }
    void OnDestroy() { Unbind(); }
    void OnDisable() {
        game?.UnbindReplica(id);
        Initialized = false;
    }
    void FixedUpdate() {
        if (game == null)
            return;
        object[] sample;
        if (body3)
            sample = forecast3.Sample(body3, TeleportGeneration, Phase);
        else if (body2)
            sample = forecast2.Sample(body2, TeleportGeneration, Phase);
        else {
            sample = new object[bindings.Length];
            for (int i = 0; i < sample.Length; i++) {
                var target = bindings[i].Target;
                if (!target)
                    return;
                sample[i] = members[i] != null ? members[i].GetValue(target) : properties[i].GetValue(target);
            }
        }
        int mode = game.SyncReplica(id, sample, out var fields, out double age, body3 || body2);
        if (mode <= 0)
            return;
        if (body3 || body2) {
            float distance = body3 ? Vector3.Distance((Vector3)sample[0], (Vector3)fields[0])
                                   : Vector2.Distance((Vector2)sample[0], (Vector2)fields[0]);
            float angle = body3 ? Quaternion.Angle((Quaternion)sample[1], (Quaternion)fields[1])
                                : Mathf.Abs(Mathf.DeltaAngle((float)sample[1] * Mathf.Rad2Deg,
                                                             (float)fields[1] * Mathf.Rad2Deg));
            TeleportGeneration = (int)fields[5];
            Phase = (int)fields[6];
            bool snap = mode == 2 || (TeleportDistance > 0 && distance > TeleportDistance) ||
                        (TeleportAngleDegrees > 0 && angle > TeleportAngleDegrees);
            if (body3) {
                if (snap)
                    forecast3.ResetBaseline(TeleportGeneration);
                forecast3.Apply(body3, fields, age);
            } else {
                if (snap)
                    forecast2.ResetBaseline(TeleportGeneration);
                forecast2.Apply(body2, fields, age);
            }
        } else {
            for (int i = 0; i < fields.Length; i++) {
                var target = bindings[i].Target;
                if (members[i] != null)
                    members[i].SetValue(target, fields[i]);
                else
                    properties[i].SetValue(target, fields[i]);
            }
        }
        Initialized = true;
    }
}
}
