package org.nativescript.staticbindinggenerator;

import org.apache.bcel.classfile.ClassParser;
import org.apache.bcel.classfile.JavaClass;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.jar.JarInputStream;
import java.util.zip.ZipEntry;

public class GetInterfaceNames {
    private static final String CLASS_EXT = ".class";
    private static String currentDir;

    /*
     * generate interfaceNames.txt file, needed for js analyzer
     * */
    public static void generateInterfaceFile(List<DataRow> rows)
            throws IOException {
        currentDir = System.getProperty("user.dir");
        String outputFileName = Main.SBG_INTERFACE_NAMES;

        PrintWriter out = ensureOutputFile(outputFileName);
        List<String> interfacesList = Collections.synchronizedList(new ArrayList<>());

        final ThrowingConsumer<DataRow> consumer = dr -> {
            String pathToDependency = dr.getRow();
            if (pathToDependency.endsWith(".jar")) {
                generateInterfaceNames(pathToDependency, interfacesList);
            } else {
                // The app's own compiled classes arrive as a directory, not a
                // jar. Skipping those meant an interface the app declares was
                // never in this list, so the parser did not recognise
                // `new com.tns.tests.InterfaceOne({...})` as implementing one
                // and no binding was pre-generated -- the class was built on
                // device instead, which works but costs a dex at startup.
                File dir = new File(pathToDependency);
                if (dir.isDirectory()) {
                    generateInterfaceNamesFromDirectory(dir, interfacesList);
                }
            }
        };

        rows.parallelStream().forEach(consumer);

        for (String line : interfacesList) {
            out.println(line);
        }

        out.close();
    }

    private static void generateInterfaceNamesFromDirectory(File dir, List<String> interfacesList) {
        File[] entries = dir.listFiles();
        if (entries == null) {
            return;
        }

        for (File entry : entries) {
            if (entry.isDirectory()) {
                generateInterfaceNamesFromDirectory(entry, interfacesList);
                continue;
            }
            if (!entry.getName().endsWith(CLASS_EXT)) {
                continue;
            }
            // Unreadable or half-written class files are skipped rather than
            // fatal: compileAppClassesForSbg is best-effort by design, so a
            // source it could not compile must not take the build down here.
            try (FileInputStream in = new FileInputStream(entry)) {
                JavaClass clazz = new ClassParser(in, entry.getName()).parse();
                if (clazz.isInterface()) {
                    interfacesList.add(clazz.getClassName().replace('$', '.'));
                }
            } catch (IOException | RuntimeException e) {
                // ignored -- see above
            }
        }
    }

    private static void generateInterfaceNames(String pathToJar, List<String> interfacesList) {
        if (pathToJar == null) {
            return;
        }
        JarInputStream jis = null;
        try {
            String name;
            jis = new JarInputStream(new FileInputStream(pathToJar));
            for (ZipEntry ze = jis.getNextEntry(); ze != null; ze = jis.getNextEntry()) {
                try {
                    name = ze.getName();
                    if (name.endsWith(CLASS_EXT)) {
                        name = name.substring(0, name.length() - CLASS_EXT.length()).replace('/', '.').replace('$', '.');
                        ClassParser cp = new ClassParser(jis, name);
                        JavaClass clazz = cp.parse();
                        if (clazz.isInterface()) {
                            String res = clazz.getClassName().replace('$', '.');
                            interfacesList.add(res);
                        }
                    }
                } catch (IOException e) {
                    throw new RuntimeException("Error while parsing class file!", e);
                }
            }
        } catch (IOException ioe) {
            throw new RuntimeException("Error while reading JAR entry!", ioe);
        } finally {
            if (jis != null) {
                try {
                    jis.close();
                } catch (IOException e) {
                    e.printStackTrace();
                }
            }
        }
    }

    public static PrintWriter ensureOutputFile(String outputFileName) throws IOException {
        File checkFile = new File(currentDir, outputFileName);
        if (checkFile.exists()) {
            checkFile.delete();
        } else {
            checkFile.getParentFile().mkdirs();
            checkFile.createNewFile();
        }

        return new PrintWriter(new BufferedWriter(new FileWriter(checkFile.getAbsolutePath(), true)));
    }
}