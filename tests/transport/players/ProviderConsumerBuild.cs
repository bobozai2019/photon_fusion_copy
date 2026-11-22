using System;
using System.IO;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class ProviderConsumerBuild
{
    public static void Export()
    {
        var args = Environment.GetCommandLineArgs();
        string output = args[Array.IndexOf(args, "-sdkOutput") + 1];
        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene);
        new GameObject("Provider consumer").AddComponent<ProviderConsumer>();
        const string path = "Assets/Smoke.unity";
        if (!EditorSceneManager.SaveScene(scene, path)) throw new IOException(path);
        var build = BuildPipeline.BuildPlayer(new[] {path}, output, BuildTarget.StandaloneWindows64, BuildOptions.None);
        if (build.summary.result != BuildResult.Succeeded) throw new Exception(build.summary.result.ToString());
    }
}
