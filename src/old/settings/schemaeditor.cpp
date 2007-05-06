/***************************************************************************
                          schemaeditor.cpp  -  description
                             -------------------
    begin                : mar apr 17 16:44:59 CEST 2001
    copyright            : (C) 2001 by Andrea Rizzi
    email                : rizzi@kde.org
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#define TABLE_COLORS 20


#include "schemaeditor.h"
#include "schemaeditor.moc"

#include <kdebug.h>
#include <kstandarddirs.h>
#include <klocale.h>
#include <kfiledialog.h>
#include <kinputdialog.h>
#include <kmessagebox.h>
#include <kimageeffect.h>

#include <QtDBus/QtDBus>

#include <QLabel>
#include <QApplication>
#include <QLineEdit>
#include <QImage>
#include <QMatrix>
#include <QComboBox>
#include <QDesktopWidget>
#include <QPixmap>
#include <QTextStream>
#include <QCheckBox>
#include <QSlider>
#include <QToolButton>

#ifdef Q_WS_X11
#include <kdesktop_background_interface.h>
#endif

// SchemaListBoxText is a list box text item with schema filename
class SchemaListBoxText : public QListWidgetItem
{
  public:
    SchemaListBoxText(const QString &title, const QString &filename): QListWidgetItem(title)
    {
      m_filename = filename;
    };

    const QString filename() { return m_filename; };

  private:
    QString m_filename;
};


SchemaEditor::SchemaEditor(QWidget * parent)
:SchemaDialog(parent)
{
    schMod= false;
    loaded = false;
    schemaLoaded = false;
    change = false;
    oldSlot = 0;
    oldSchema = -1;
    color.resize(20);
    type.resize(20);
    bold.resize(20);
    transparent.resize(20);
    defaultSchema = "";
#ifdef Q_WS_X11
    int konq_screen_number = QApplication::desktop()->primaryScreen();
    QByteArray appname;
    if (konq_screen_number == 0)
        appname = "org.kde.kdesktop";
    else
        appname = "org.kde.kdesktop-screen-" + QByteArray::number( konq_screen_number);
    org::kde::kdesktop::Background desktop(appname, "/Background", QDBusConnection::sessionBus());
    desktop.setExport(1);
    
#endif
    transparencyCheck->setChecked(true);
    transparencyCheck->setChecked(false);


    KGlobal::locale()->insertCatalog("konsole"); // For schema translations
    connect(imageBrowse, SIGNAL(clicked()), this, SLOT(imageSelect()));
    connect(saveButton, SIGNAL(clicked()), this, SLOT(saveCurrent()));
    connect(removeButton, SIGNAL(clicked()), this, SLOT(removeCurrent()));
    connect(colorCombo, SIGNAL(activated(int)), this, SLOT(slotColorChanged(int)));
    connect(typeCombo, SIGNAL(activated(int)), this, SLOT(slotTypeChanged(int)));
    connect(schemaList, SIGNAL(highlighted(int)), this, SLOT(readSchema(int)));
    connect(shadeColor, SIGNAL(changed(const QColor&)), this, SLOT(updatePreview()));
    connect(shadeSlide, SIGNAL(valueChanged(int)), this, SLOT(updatePreview()));
    connect(transparencyCheck, SIGNAL(toggled(bool)), this, SLOT(updatePreview()));
    connect(backgndLine, SIGNAL(returnPressed()), this, SLOT(updatePreview()));

    connect(titleLine, SIGNAL(textChanged(const QString&)), this, SLOT(schemaModified()));
    connect(shadeColor, SIGNAL(changed(const QColor&)), this, SLOT(schemaModified()));
    connect(shadeSlide, SIGNAL(valueChanged(int)), this, SLOT(schemaModified()));
    connect(transparencyCheck, SIGNAL(toggled(bool)), this, SLOT(schemaModified()));
    connect(modeCombo, SIGNAL(activated(int)), this, SLOT(schemaModified()));
    connect(backgndLine, SIGNAL(returnPressed()), this, SLOT(schemaModified()));
    connect(transparentCheck, SIGNAL(toggled(bool)), this, SLOT(schemaModified()));
    connect(boldCheck, SIGNAL(toggled(bool)), this, SLOT(schemaModified()));
    connect(colorButton, SIGNAL(changed(const QColor&)), this, SLOT(schemaModified()));
    connect(backgndLine, SIGNAL(textChanged(const QString&)), this, SLOT(schemaModified()));

    connect(defaultSchemaCB, SIGNAL(toggled(bool)), this, SIGNAL(changed()));
    removeButton->setEnabled( schemaList->currentItem() );
    load();
}


QString SchemaEditor::schema()
{
    QString filename = defaultSchema;

    int i = schemaList->currentRow();
    if (defaultSchemaCB->isChecked() && i>=0)
      filename = ((SchemaListBoxText *) schemaList->item(i))->filename();

    return filename.section('/',-1);
}


void SchemaEditor::setSchema(QString sch)
{
    defaultSchema = sch;
    sch = KStandardDirs::locate("data", "konsole/"+sch);

    int sc = -1;
    for (int i = 0; i < (int) schemaList->count(); i++)
	if (sch == ((SchemaListBoxText *) schemaList->item(i))->filename())
	    sc = i;

    oldSchema = sc;
    if (sc == -1)
	sc = 0;
    schemaList->setCurrentRow(sc);
//    readSchema(sc);
}

SchemaEditor::~SchemaEditor()
{
}



void SchemaEditor::updatePreview()
{

    if (transparencyCheck->isChecked()) {
	if (loaded) {
	    float rx = (100.0 - shadeSlide->value()) / 100;
	    QImage ima(pix.toImage());
	    ima = KImageEffect::fade(ima, rx, shadeColor->color());
	    QPixmap pm = QPixmap::fromImage( ima );
	    previewPixmap->setPixmap(pm);
	    previewPixmap->setScaledContents(true);
	}
	 else  //try to reload
	{
    QMatrix mat;
    QDesktopWidget desktopWidget;
    QPixmap pixmap = QPixmap::grabWidget( desktopWidget.screen( 0 ) );

    pix = pixmap.transformed( mat.scale(180.0 / pixmap.width(),
                              100.0 / pixmap.height()) );

    kDebug(0) << "Loaded" << endl;
    loaded = true;
    if (transparencyCheck->isChecked()) {
      updatePreview();
    }
	}
      } else {
	QPixmap pm;
	pm.load(backgndLine->text());
	if ( pm.isNull() ) {
	    previewPixmap->clear();
	} else {
	    previewPixmap->setPixmap(pm);
	    previewPixmap->setScaledContents(true);
	}
    }

}

void SchemaEditor::getList()
{
    if (! schemaLoaded) {
	loadAllSchema();
	setSchema(defaultSchema);
	schemaLoaded = true;
	change = true;
    }
}

void SchemaEditor::load()
{
    getList();
}


void SchemaEditor::loadAllSchema(QString currentFile)
{
    QStringList list = KGlobal::dirs()->findAllResources("data", "konsole/*.schema");
    QStringList::ConstIterator it;
    disconnect(schemaList, SIGNAL(highlighted(int)), this, SLOT(readSchema(int)));
    schemaList->clear();

    QListWidgetItem* currentItem = 0;
    for (it = list.begin(); it != list.end(); ++it) {

	QString name = (*it);

	QString title = readSchemaTitle(name);

	// Only insert new items so that local items override global
	if (schemaList->findItems(title, Qt::MatchExactly).isEmpty()) {
	    if (title.isNull() || title.isEmpty())
		title=i18n("untitled");

		schemaList->addItem(new SchemaListBoxText(title, name));
	    if (currentFile==name.section('/',-1))
                currentItem = schemaList->item( schemaList->count()-1 );
	}
    }
    schemaList->model()->sort(0);
    schemaList->setCurrentRow(0);   // select the first added item correctly too
    schemaList->setCurrentItem(currentItem);

    //FIXME - This signal/slot is broken following Q3ListBox -> QListWidget change
    connect(schemaList, SIGNAL(highlighted(int)), this, SLOT(readSchema(int)));
    schemaListChanged();
}

void SchemaEditor::imageSelect()
{
    QString start;
    start = backgndLine->text();
    if (start.isEmpty())
    {
        QStringList list=KGlobal::dirs()->resourceDirs("wallpaper");
        if(list.count()>0)
            start= list.last();
    }

    KUrl url = KFileDialog::getImageOpenUrl(start, 0, i18n("Select Background Image"));
// KUrl url=KFileDialog::getOpenURL(start,"",0,i18n("Select Background Image"));
    if(!url.path().isEmpty())
    {
        backgndLine->setText(url.path());
        updatePreview();
    }
}

void SchemaEditor::slotTypeChanged(int slot)
{
    schemaModified();

    bool active = slot == 0 || slot == 3;
    colorButton->setEnabled(active);
    boldCheck->setEnabled(active);
    transparentCheck->setEnabled(active);
}


void SchemaEditor::slotColorChanged(int slot)
{
    kDebug(0) << slot << endl;
    color[oldSlot] = colorButton->color();
    type[oldSlot] = typeCombo->currentIndex();
    bold[oldSlot] = boldCheck->isChecked();
    transparent[oldSlot] = transparentCheck->isChecked();

    change = false; // Don't mark as modified
    transparentCheck->setChecked(transparent[slot]);
    boldCheck->setChecked(bold[slot]);
    typeCombo->setCurrentIndex(type[slot]);
    colorButton->setColor(color[slot]);
    oldSlot = slot;
    change = true;
}

void SchemaEditor::removeCurrent()
{
    int i = schemaList->currentRow();
    if(i==-1)
        return;
    QString base = ((SchemaListBoxText *) schemaList->item(i))->filename();

    // Query if system schemas should be removed
    if (KStandardDirs::locateLocal("data", "konsole/" + base.section('/', -1)) != base) {
	int code = KMessageBox::warningContinueCancel(this,
	    i18n("You are trying to remove a system schema. Are you sure?"),
	    i18n("Removing System Schema"),
	    KStandardGuiItem::del());
	if (code != KMessageBox::Continue)
	    return;
    }

    QString base_filename = base.section('/',-1);

    if(base_filename==schema())
     setSchema("");

    if (!QFile::remove(base))
	KMessageBox::error(this,
			   i18n("Cannot remove the schema.\nMaybe it is a system schema.\n"),
			   i18n("Error Removing Schema"));

    loadAllSchema();

    setSchema(defaultSchema);

}

void SchemaEditor::saveCurrent()
{

    //This is to update the color table
    colorCombo->setCurrentIndex(0);
    slotColorChanged(0);

    QString fullpath;
    if (schemaList->currentItem()->text() == titleLine->text()) {
	int i = schemaList->currentRow();
	fullpath = ((SchemaListBoxText *) schemaList->item(i))->filename().section('/',-1);
    }
    else {
	// Only ask for a name for changed titleLine, considered a "save as"
	fullpath = titleLine->text().trimmed().simplified()+".schema";

    bool ok;
    fullpath = KInputDialog::getText( i18n( "Save Schema" ),
        i18n( "File name:" ), fullpath, &ok, this );
	if (!ok) return;
    }

    if (fullpath[0] != '/')
        fullpath = KGlobal::dirs()->saveLocation("data", "konsole/") + fullpath;

    QFile f(fullpath);
    if (f.open(QIODevice::WriteOnly)) {
	QTextStream t(&f);
        t.setCodec( "UTF-8" );

	t << "# schema for konsole autogenerated with the schema editor" << endl;
	t << endl;
	t << "title " << titleLine->text() << endl; // Use title line as schema title
	t << endl;
	if (transparencyCheck->isChecked()) {
	    QColor c = shadeColor->color();
	    QString tra;
	    tra.sprintf("transparency %1.2f %3d %3d %3d",
			1.0 * (100 - shadeSlide->value()) / 100, c.red(), c.green(), c.blue());
	    t << tra << endl;
	}

	if (!backgndLine->text().isEmpty()) {
	    QString smode;
	    int mode;
	    mode = modeCombo->currentIndex();
	    if (mode == 0)
		smode = "tile";
	    if (mode == 1)
		smode = "center";
	    if (mode == 2)
		smode = "full";

	    QString image;
	    image.sprintf("image %s %s",
			  smode.toLatin1().constData(),
			  backgndLine->text().toUtf8().constData());
	    t << image << endl;
	}
	t << endl;
	t << "# foreground colors" << endl;
	t << endl;
	t << "# note that the default background color is flagged" << endl;
	t << "# to become transparent when an image is present." << endl;
	t << endl;
	t << "#   slot    transparent bold" << endl;
	t << "#      | red grn blu  | |" << endl;
	t << "#      V V--color--V  V V" << endl;

	for (int i = 0; i < 20; i++) {
	    QString scol;
	    if (type[i] == 0)
		scol.sprintf("color %2d %3d %3d %3d %2d %1d # %s", i,
			     color[i].red(), color[i].green(), color[i].blue(),
			     transparent[i], bold[i],
			     (const char *) colorCombo->itemText(i).toUtf8());
	    else if (type[i] == 1)
		scol.sprintf("sysfg %2d             %2d %1d # %s", i,
			     transparent[i], bold[i],
			     (const char *) colorCombo->itemText(i).toUtf8());
	    else if (type[i] == 2)
		scol.sprintf("sysbg %2d             %2d %1d # %s", i,
			     transparent[i], bold[i],
			     (const char *) colorCombo->itemText(i).toUtf8());
	    else {
		int ch, cs, cv;
		color[i].getHsv(&ch, &cs, &cv);
		scol.sprintf("rcolor %1d %3d %3d     %2d %1d # %s", i,
			     cs, cv, transparent[i], bold[i],
			     (const char *) colorCombo->itemText(i).toUtf8());
	    }
	    t << scol << endl;
	}


	f.close();
    } else
	KMessageBox::error(this, i18n("Cannot save the schema.\nMaybe permission denied.\n"),
			   i18n("Error Saving Schema"));

    schMod=false;
    loadAllSchema(fullpath.section('/',-1));
}

void SchemaEditor::schemaModified()
{
    if (change) {
	saveButton->setEnabled(titleLine->text().length() != 0);
	schMod=true;
	emit changed();
    }
}

QString SchemaEditor::readSchemaTitle(const QString & file)
{
    /*
       Code taken from konsole/konsole/schema.cpp

     */


    QString fPath = KStandardDirs::locate("data", "konsole/" + file);

    if (fPath.isNull())
	fPath = KStandardDirs::locate("data", file);

    if (fPath.isNull())
	return 0;

    FILE *sysin = fopen(QFile::encodeName(fPath), "r");
    if (!sysin)
	return 0;


    char line[100];
    while (fscanf(sysin, "%80[^\n]\n", line) > 0)
	if (strlen(line) > 5)
	    if (!strncmp(line, "title", 5)) {
		fclose(sysin);
		return i18n(line + 6);
	    }

    fclose(sysin);

    return 0;
}

void SchemaEditor::schemaListChanged()
{
    QStringList titles, filenames;
    SchemaListBoxText *item;

    for (int index = 0; index < (int) schemaList->count(); index++) {
      item = (SchemaListBoxText *) schemaList->item(index);
      titles.append(item->text());
      filenames.append(item->filename().section('/', -1));
    }

    emit schemaListChanged(titles, filenames);
}

void SchemaEditor::querySave()
{
    int result = KMessageBox::questionYesNo(this,
                         i18n("The schema has been modified.\n"
			"Do you want to save the changes?"),
			i18n("Schema Modified"),
			KStandardGuiItem::save(),
			KStandardGuiItem::discard());
    if (result == KMessageBox::Yes)
    {
        saveCurrent();
    }
}

void SchemaEditor::readSchema(int num)
{
    /*
       Code taken from konsole/konsole/schema.cpp

     */

    if (oldSchema != -1) {


	if (defaultSchemaCB->isChecked()) {

	    defaultSchema = ((SchemaListBoxText *) schemaList->item(oldSchema))->filename();

	}

	if(schMod) {
	    disconnect(schemaList, SIGNAL(highlighted(int)), this, SLOT(readSchema(int)));
	    schemaList->setCurrentRow(oldSchema);
	    querySave();
	    schemaList->setCurrentRow(num);
	    connect(schemaList, SIGNAL(highlighted(int)), this, SLOT(readSchema(int)));
	    schMod=false;
	}

    }

    QString fPath = KStandardDirs::locate("data", "konsole/" +
			   ((SchemaListBoxText *) schemaList->item(num))->filename());

    if (fPath.isNull())
	fPath = KStandardDirs::locate("data",
		       ((SchemaListBoxText *) schemaList->item(num))->filename());

    if (fPath.isNull()) {
	KMessageBox::error(this, i18n("Cannot find the schema."),
			   i18n("Error Loading Schema"));


	return;
    }
    removeButton->setEnabled( QFileInfo (fPath).isWritable () );
    defaultSchemaCB->setChecked(fPath.section('/',-1) == defaultSchema.section('/',-1));

    FILE *sysin = fopen(QFile::encodeName(fPath), "r");
    if (!sysin) {
	KMessageBox::error(this, i18n("Cannot load the schema."),
			   i18n("Error Loading Schema"));
	loadAllSchema();
	return;
    }

    char line[100];


    titleLine->setText(i18n("untitled"));
    transparencyCheck->setChecked(false);
    backgndLine->setText("");

    while (fscanf(sysin, "%80[^\n]\n", line) > 0) {
	if (strlen(line) > 5) {

	    if (!strncmp(line, "title", 5)) {
		titleLine->setText(i18n(line + 6));
	    }



	    if (!strncmp(line, "image", 5)) {
		char rend[100], path[100];
		int attr = 1;
		if (sscanf(line, "image %s %s", rend, path) != 2)
		    continue;
		if (!strcmp(rend, "tile"))
		    attr = 2;
		else if (!strcmp(rend, "center"))
		    attr = 3;
		else if (!strcmp(rend, "full"))
		    attr = 4;
		else
		    continue;

		QString qline(line);
		backgndLine->setText(KStandardDirs::locate("wallpaper", qline.mid( qline.indexOf(" ",7)+1 ) ));
		modeCombo->setCurrentIndex(attr - 2);

	    }


	    if (!strncmp(line, "transparency", 12)) {
		float rx;
		int rr, rg, rb;
		// Transparency needs 4 parameters: fade strength and the 3
		// components of the fade color.
		if (sscanf(line, "transparency %g %d %d %d", &rx, &rr, &rg, &rb) != 4)
		    continue;

		transparencyCheck->setChecked(true);
		shadeSlide->setValue((int)(100 - rx * 100));
		shadeColor->setColor(QColor(rr, rg, rb));

	    }
            if (!strncmp(line,"rcolor",6)) {
                int fi,ch,cs,cv,tr,bo;
                if(sscanf(line,"rcolor %d %d %d %d %d",&fi,&cs,&cv,&tr,&bo) != 5)
                    continue;
                if (!(0 <= fi && fi <= TABLE_COLORS))
                    continue;
                ch = 0; // Random hue - set to zero
                if (!(0 <= cs && cs <= 255         ))
                    continue;
                if (!(0 <= cv && cv <= 255         ))
                    continue;
                if (!(0 <= tr && tr <= 1           ))
                    continue;
                if (!(0 <= bo && bo <= 1           ))
                    continue;
                color[fi] = QColor();
                color[fi].setHsv(ch,cs,cv);
                transparent[fi] = tr;
                bold[fi] = bo;
                type[fi] = 3;
            }
	    if (!strncmp(line, "color", 5)) {
		int fi, cr, cg, cb, tr, bo;
		if (sscanf(line, "color %d %d %d %d %d %d", &fi, &cr, &cg, &cb, &tr, &bo) != 6)
		    continue;
		if (!(0 <= fi && fi <= TABLE_COLORS))
		    continue;
		if (!(0 <= cr && cr <= 255))
		    continue;
		if (!(0 <= cg && cg <= 255))
		    continue;
		if (!(0 <= cb && cb <= 255))
		    continue;
		if (!(0 <= tr && tr <= 1))
		    continue;
		if (!(0 <= bo && bo <= 1))
		    continue;
		color[fi] = QColor(cr, cg, cb);
		transparent[fi] = tr;
		bold[fi] = bo;
		type[fi] = 0;

	    }
	    if (!strncmp(line, "sysfg", 5)) {
		int fi, tr, bo;
		if (sscanf(line, "sysfg %d %d %d", &fi, &tr, &bo) != 3)
		    continue;
		if (!(0 <= fi && fi <= TABLE_COLORS))
		    continue;
		if (!(0 <= tr && tr <= 1))
		    continue;
		if (!(0 <= bo && bo <= 1))
		    continue;
		color[fi] = qApp->palette().color(QPalette::Active,QPalette::Text);
		transparent[fi] = tr;
		bold[fi] = bo;
		type[fi] = 1;
	    }
	    if (!strncmp(line, "sysbg", 5)) {
		int fi, tr, bo;
		if (sscanf(line, "sysbg %d %d %d", &fi, &tr, &bo) != 3)
		    continue;
		if (!(0 <= fi && fi <= TABLE_COLORS))
		    continue;
		if (!(0 <= tr && tr <= 1))
		    continue;
		if (!(0 <= bo && bo <= 1))
		    continue;
		color[fi] = qApp->palette().color(QPalette::Active,QPalette::Base);
		transparent[fi] = tr;
		bold[fi] = bo;
		type[fi] = 2;
	    }
	}
    }
    fclose(sysin);
    int ii = colorCombo->currentIndex();
    transparentCheck->setChecked(transparent[ii]);
    boldCheck->setChecked(bold[ii]);
    typeCombo->setCurrentIndex(type[ii]);
    colorButton->setColor(color[ii]);

    bool inactive = type[ii] == 1 || type[ii] == 2;
    boldCheck->setDisabled(inactive);
    transparentCheck->setDisabled(inactive);
    colorButton->setDisabled(inactive);

    oldSchema = num;
    updatePreview();
    schMod=false;
    return;
}
