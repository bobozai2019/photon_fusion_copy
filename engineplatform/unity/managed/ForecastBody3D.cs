using UnityEngine;

namespace Netcode {
// Wire basis is right-handed, with Y up. Angular velocity is an axial vector.
public sealed class ForecastBody3D {
    public void ResetBaseline(int next) { generation=~next; }
    public ForecastCorrection Mode;
    public float MaxForecast = .25f, Stiffness = 50, Damping = 10, BlendFactor = .1f;
    public bool IncludeGravity = true;
    int generation = -1;
    static Vector3 Polar(Vector3 v) => new Vector3(v.x, v.y, -v.z);
    static Vector3 Axial(Vector3 w) => new Vector3(-w.x, -w.y, w.z);
    static Quaternion Basis(Quaternion q) => new Quaternion(-q.x, -q.y, q.z, q.w);
    public object[] Sample(Rigidbody body, int teleport,
                           int phase) => new object[] { Polar(body.position),
                                                        Basis(body.rotation),
                                                        Polar(body.linearVelocity),
                                                        Axial(body.angularVelocity),
                                                        body.IsSleeping(),
                                                        teleport,
                                                        phase };
    public void Apply(Rigidbody body, object[] fields, double elapsed) {
        bool snap = generation != (int)fields[5] || (bool)fields[4];
        generation = (int)fields[5];
        float age = snap ? 0 : Mathf.Clamp((float)elapsed, 0, Mathf.Max(0, MaxForecast));
        var velocity = Polar((Vector3)fields[2]);
        var spin = Axial((Vector3)fields[3]);
        var gravity = IncludeGravity && body.useGravity ? Physics.gravity : Vector3.zero;
        var target = Polar((Vector3)fields[0]) + velocity * age + gravity * (.5f * age * age);
        velocity += gravity * age;
        var rotation = Basis((Quaternion)fields[1]);
        if (spin.sqrMagnitude > 1e-6f)
            rotation = Quaternion.AngleAxis(spin.magnitude * age * Mathf.Rad2Deg, spin.normalized) * rotation;
        var error = rotation * Quaternion.Inverse(body.rotation);
        error.ToAngleAxis(out float degrees, out Vector3 axis);
        var angular = Mathf.Abs(degrees) < .0001f ? Vector3.zero
                                                  : axis * (Mathf.DeltaAngle(0, degrees) * Mathf.Deg2Rad);
        body.WakeUp();
        if (snap) {
            body.position = target;
            body.rotation = rotation;
            body.linearVelocity = velocity;
            body.angularVelocity = spin;
        } else if (Mode == ForecastCorrection.Blend) {
            body.position = Vector3.Lerp(body.position, target, BlendFactor);
            body.rotation = Quaternion.Slerp(body.rotation, rotation, BlendFactor);
            body.linearVelocity = velocity;
            body.angularVelocity = spin;
        } else if (Mode == ForecastCorrection.Force) {
            body.AddForce(Stiffness * (target - body.position) + Damping * (velocity - body.linearVelocity),
                          ForceMode.Acceleration);
            body.AddTorque(Stiffness * angular + Damping * (spin - body.angularVelocity),
                           ForceMode.Acceleration);
        } else {
            body.linearVelocity = velocity + 6 * (target - body.position);
            body.angularVelocity = spin + 6 * angular;
        }
        if ((bool)fields[4])
            body.Sleep();
    }
}
}
