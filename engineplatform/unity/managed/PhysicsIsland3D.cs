using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Netcode.Unity
{
    // The caller owns the isolated scene, its static geometry, joint configuration and input history.
    // Every dynamic body in the interaction group must have a stable network object ID.
    public sealed class PhysicsIsland3D
    {
        public struct BodyState
        {
            public uint Id;
            public Vector3 Position, Velocity, AngularVelocity;
            public Quaternion Rotation;
            public bool Sleeping;
        }
        public sealed class Snapshot
        {
            public readonly uint Tick;
            public readonly IReadOnlyList<BodyState> Bodies;
            // Schema order: tick, stable IDs, sleeping flags, packed position/rotation/linear/angular velocity.
            public object[] Fields()
            {
                var ids = new int[Bodies.Count];
                var flags = new int[Bodies.Count];
                var poses = new float[checked(Bodies.Count*13)];
                for (int i=0;i<Bodies.Count;i++) {
                    var s = Bodies[i];
                    ids[i] = unchecked((int)s.Id); flags[i] = s.Sleeping ? 1 : 0;
                    int j = i*13;
                    poses[j]=s.Position.x; poses[j+1]=s.Position.y; poses[j+2]=s.Position.z;
                    poses[j+3]=s.Rotation.x; poses[j+4]=s.Rotation.y; poses[j+5]=s.Rotation.z; poses[j+6]=s.Rotation.w;
                    poses[j+7]=s.Velocity.x; poses[j+8]=s.Velocity.y; poses[j+9]=s.Velocity.z;
                    poses[j+10]=s.AngularVelocity.x; poses[j+11]=s.AngularVelocity.y; poses[j+12]=s.AngularVelocity.z;
                }
                return new object[]{unchecked((int)Tick),ids,flags,poses};
            }
            public static Snapshot FromFields(object[] fields)
            {
                if (fields == null || fields.Length != 4 || !(fields[0] is int tick) ||
                    !(fields[1] is int[] ids) || !(fields[2] is int[] flags) || !(fields[3] is float[] p) ||
                    ids.Length != flags.Length || p.Length % 13 != 0 || ids.Length != p.Length/13) return null;
                var states = new BodyState[ids.Length];
                for (int i=0;i<ids.Length;i++) {
                    if (flags[i] != 0 && flags[i] != 1) return null;
                    int j=i*13;
                    states[i] = new BodyState { Id=unchecked((uint)ids[i]), Sleeping=flags[i]==1,
                        Position=new Vector3(p[j],p[j+1],p[j+2]), Rotation=new Quaternion(p[j+3],p[j+4],p[j+5],p[j+6]),
                        Velocity=new Vector3(p[j+7],p[j+8],p[j+9]), AngularVelocity=new Vector3(p[j+10],p[j+11],p[j+12]) };
                }
                return new Snapshot(unchecked((uint)tick),states);
            }
            public Snapshot(uint tick, IEnumerable<BodyState> bodies)
            {
                Tick = tick;
                Bodies = new List<BodyState>(bodies).AsReadOnly();
            }
        }
        readonly SortedDictionary<uint,Rigidbody> bodies = new SortedDictionary<uint,Rigidbody>();
        readonly PhysicsScene world;
        readonly float dt;
        public uint Tick { get; private set; }

        public PhysicsIsland3D(Scene scene, float stepSeconds)
        {
            world = scene.GetPhysicsScene();
            if (!world.IsValid() || world == Physics.defaultPhysicsScene ||
                !float.IsFinite(stepSeconds) || stepSeconds <= 0)
                throw new ArgumentException("An isolated 3D physics scene and positive step are required");
            dt = stepSeconds;
        }

        public void Add(uint id, Rigidbody body)
        {
            if (id == 0 || !body || body.isKinematic || body.gameObject.scene.GetPhysicsScene() != world ||
                bodies.ContainsValue(body)) throw new ArgumentException("Invalid island body");
            bodies.Add(id, body);
        }

        public Snapshot Capture()
        {
            var states = new List<BodyState>(bodies.Count);
            foreach (var pair in bodies) {
                var b = pair.Value;
                states.Add(new BodyState { Id = pair.Key, Position = b.position, Rotation = b.rotation,
                    Velocity = b.linearVelocity, AngularVelocity = b.angularVelocity, Sleeping = b.IsSleeping() });
            }
            return new Snapshot(Tick, states);
        }

        static bool Finite(Vector3 v) => float.IsFinite(v.x) && float.IsFinite(v.y) && float.IsFinite(v.z);
        public bool Restore(Snapshot image)
        {
            if (image == null || image.Bodies.Count != bodies.Count) return false;
            var seen = new HashSet<uint>();
            foreach (var s in image.Bodies) {
                float norm = Quaternion.Dot(s.Rotation,s.Rotation);
                if (!bodies.TryGetValue(s.Id,out var b) || !b || !seen.Add(s.Id) ||
                    !Finite(s.Position) || !Finite(s.Velocity) || !Finite(s.AngularVelocity) ||
                    !float.IsFinite(norm) || Mathf.Abs(norm-1) > .001f) return false;
            }
            foreach (var s in image.Bodies) {
                var b = bodies[s.Id];
                b.position = s.Position;
                b.rotation = s.Rotation;
                b.linearVelocity = s.Velocity;
                b.angularVelocity = s.AngularVelocity;
                if (s.Sleeping) b.Sleep(); else b.WakeUp();
            }
            Tick = image.Tick;
            return true;
        }

        public void Step(Action<uint> applyInput)
        {
            uint next = checked(Tick + 1);
            applyInput?.Invoke(next);
            world.Simulate(dt);
            Tick = next;
        }
    }
}
