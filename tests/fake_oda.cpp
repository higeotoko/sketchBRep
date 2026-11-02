// Controlled process fixture, not a DWG decoder. Checks the ODA CLI contract.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThread>
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const auto a = app.arguments();
  if (a.size() == 8 && a[3] == "ACAD2018" && a[4] == "DWG" && a[5] == "0" && a[6] == "1" && a[7] == "drawing.dxf") {
    // DXF -> DWG (drawing export): a DWG header in front of the DXF it got.
    QFile input(QDir(a[1]).filePath(a[7]));
    if (!input.open(QIODevice::ReadOnly)) return 4;
    const auto data = input.readAll();
    if (!data.contains("EOF")) return 5;
    QFile output(QDir(a[2]).filePath("drawing.dwg"));
    if (!output.open(QIODevice::WriteOnly)) return 6;
    output.write("AC1032");
    output.write(data);
    return 0;
  }
  if (a.size() != 8 || a[3] != "ACAD2018" || a[4] != "DXF" || a[5] != "0" || a[6] != "1" || a[7] != "drawing.dwg") return 2;
  if (QDir(a[1]).entryList(QDir::Files).size() != 1) return 3;
  QFile input(QDir(a[1]).filePath(a[7]));
  if (!input.open(QIODevice::ReadOnly)) return 4;
  const auto data = input.readAll();
  if (data.contains("FAIL")) return 5;
  if (data.contains("TIMEOUT")) QThread::sleep(10);
  if (data.contains("MISSING")) return 0;
  QFile output(QDir(a[2]).filePath("drawing.dxf"));
  if (!output.open(QIODevice::WriteOnly)) return 6;
  output.write("0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"
               "0\nLINE\n8\nWalls\n10\n0\n20\n0\n11\n100\n21\n0\n0\nENDSEC\n0\nEOF\n");
  return 0;
}
