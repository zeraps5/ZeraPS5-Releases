# SPDX-License-Identifier: GPL-2.0-or-later
# Local, transactional installer. Does not download, elevate, or stop processes.
param(
    [Parameter(Mandatory=$true)][string]$InstallRoot,
    [string]$Package,
    [string]$ExpectedSha256,
    [int]$ExpectedBuild = 0,
    [string]$Language,
    [int]$WaitPid = 0,
    [switch]$Restart,
    [switch]$Recover,
    [ValidateRange(0,120)][int]$FileLockTimeoutSeconds = 30,
    [int]$TestFailAfter = 0,
    [int]$TestInterruptAfter = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
Add-Type -AssemblyName System.IO.Compression.FileSystem
# BEGIN GENERATED UPDATE TRANSLATIONS
$script:UpdateMessages = @'
{"InstallRoot must contain ZeraPS5.exe":{"en":"InstallRoot must contain ZeraPS5.exe","pt-BR":"O InstallRoot deve conter ZeraPS5.exe","es":"InstallRoot debe contener ZeraPS5.exe","fr":"InstallRoot doit contenir ZeraPS5.exe","de":"InstallRoot muss ZeraPS5.exe enthalten","it":"InstallRoot deve contenere ZeraPS5.exe","ja":"InstallRoot には ZeraPS5.exe が含まれなければなりません。","ko":"InstallRoot에는 ZeraPS5.exe가 포함되어야 합니다.","zh-CN":"InstallRoot 必须包含 ZeraPS5.exe","ar":"يجب أن يحتوي InstallRoot على ZeraPS5.exe","fa":"InstallRoot باید حاوی ZeraPS5.exe باشد.","ru":"InstallRoot должен содержать ZeraPS5.exe","uk":"InstallRoot має містити ZeraPS5.exe","pt-PT":"O InstallRoot deve conter ZeraPS5.exe","zh-TW":"InstallRoot 必須包含 ZeraPS5.exe"},"Path escaped installation":{"en":"Path escaped installation","pt-BR":"Path escapado instalação","es":"Pase escapado de instalación","fr":"L’installation échappée","de":"Weg entfliehen Installation","it":"L’installazione scappata","ja":"避難インストール","ko":"도로 탈출 설치","zh-CN":"路逃安装","ar":"الطريق الهروب من التثبيت","fa":"راه فرار از راه نصب","ru":"Пут избеженная установка","uk":"Пут уникне установки","pt-PT":"Path escapado instalação","zh-TW":"路逃安裝"},"Reparse points are not update destinations":{"en":"Reparse points are not update destinations","pt-BR":"Os pontos de correção não são os destinos atualizados","es":"Los puntos de reparación no actualizan los destinos","fr":"Les points de réparation ne sont pas des destinations mises à jour","de":"Reparate Punkte sind keine Aktualisierung von Zielen","it":"I punti riparati non sono aggiornati destinazioni","ja":"修理ポイントは目的地を更新しない","ko":"수리 포인트는 목적지 업데이트되지 않습니다","zh-CN":"修复点不更新目的地","ar":"النقاط المعدلة لا تحديث الوجهات","fa":"نقطه های اصلاح شده به روز رسانی مقصد نیستند","ru":"Ремонт пунктов не обновляется по направлению","uk":"Поправки не оновлюють місця призначення","pt-PT":"Os pontos de correção não são os destinos atualizados","zh-TW":"修復點不更新目的地"},"Invalid recovery journal":{"en":"Invalid recovery journal","pt-BR":"Diário de recuperação Invalida","es":"Periódico de recuperación Invalida","fr":"Journal de récupération invalide","de":"Invalid Recovery Journal","it":"Il giornale di recupero invalido","ja":"障害回復記録","ko":"장애 복구 기록","zh-CN":"残疾回归日记","ar":"مجلة إعادة التأهيل المعاق","fa":"مجلات بازسازی معلول","ru":"Журнал инвалидной реабилитации","uk":"Журнал з відновлення інвалідів","pt-PT":"Diário de recuperação Invalida","zh-TW":"殘疾迴歸日記"},"Recovery backup is damaged":{"en":"Recovery backup is damaged","pt-BR":"Recuperação de backup está danificada","es":"La recuperación de backup está dañada","fr":"La récupération de la sauvegarde est endommagée","de":"Recovery Backup ist beschädigt","it":"Il backup di recupero è danneggiato","ja":"復元バックアップが損傷","ko":"복구 백업이 손상되었습니다.","zh-CN":"恢复备份已损坏","ar":"استرداد النسخ الاحتياطي تضر","fa":"بازسازی پشتیبان آسیب دیده است","ru":"Реабилитация резервной копии повреждена","uk":"Реабілітація резервного копіювання пошкоджена","pt-PT":"Recuperação de backup está danificada","zh-TW":"恢復備份已損壞"},"WaitPid is not this installation launcher":{"en":"WaitPid is not this installation launcher","pt-BR":"WaitPid não é este lançador de instalação","es":"WaitPid no es este lanzador de instalación","fr":"WaitPid n'est pas ce lanceur d'installation","de":"WaitPid ist nicht dieser Installation Luncher","it":"WaitPid non è questo lanciatore di installazione","ja":"WaitPid はこのインストール ラウンチャーではありません","ko":"WaitPid 이 설치 라운터가 아닙니다","zh-CN":"WaitPid 不是此安装启动器","ar":"WaitPid ليس هذا المبدع التثبيت","fa":"WaitPid این راه اندازی نصب نیست","ru":"WaitPid не является этой установкой запуска","uk":"WaitPid не є цим запускником установки","pt-PT":"WaitPid não é este lançador de instalação","zh-TW":"WaitPid 不是此安裝啓動器"},"Launcher did not close; no files were replaced":{"en":"Launcher did not close; no files were replaced","pt-BR":"O lançador não foi fechado; nenhum arquivo foi substituído","es":"El lanzador no se cerró; no se sustituyeron archivos","fr":"Le lanceur n'a pas été fermé; aucun fichier n'a été remplacé","de":"Startschalter wurde nicht geschlossen; keine Dateien wurden ersetzt","it":"Il lanciatore non si è chiuso; nessun file è stato sostituito","ja":"リリースは閉鎖されず、ファイルは置き換えられていません。","ko":"런치가 닫히지 않았습니다; 파일이 대체되지 않았습니다.","zh-CN":"发行器没有关闭;没有文件被取代","ar":"لم يتم إغلاق الملفات؛ لم يتم استبدال أي ملفات","fa":"راه اندازی بسته نشد؛ هیچ فایل جایگزین نشد","ru":"Старт не закрылся; никаких файлов не заменялось","uk":"Запуск не закрився; файли не були замінені","pt-PT":"O lançador não foi fechado; nenhum ficheiro foi substituído","zh-TW":"發行器沒有關閉;沒有文件被取代"},"Close games and launcher in this installation before updating":{"en":"Close games and launcher in this installation before updating","pt-BR":"Close jogos e lance nesta instalação antes de atualizar","es":"Cerca de juegos y lanza en esta instalación antes de la actualización","fr":"Close les jeux et lance dans cette installation avant l'actualisation","de":"Schließen Sie vor der Aktualisierung alle Spiele und den Launcher dieser Installation","it":"Prendi i giochi e lanci in questa installazione prima di aggiornare","ja":"ゲームを閉じ、更新前にこのインストールで起動します。","ko":"업데이트 전에 게임을 닫고 이 설치에서 시작합니다.","zh-CN":"游戏接近并在此安装之前启动","ar":"قريب من الألعاب وإطلاق في هذا التثبيت قبل التحديث","fa":"بازی ها را نزدیک کنید و قبل از به روز رسانی در این نصب راه اندازی کنید","ru":"Закрыть игры и запустить в этой установке до обновления","uk":"Залишити ігри і запустити в цій установці перед оновленням","pt-PT":"Close jogos e lance nesta instalação antes de atualizar","zh-TW":"遊戲接近並在此安裝之前啓動"},"Recovered. Saves, settings, games and system libraries were preserved.":{"en":"Recovered. Saves, settings, games and system libraries were preserved.","pt-BR":"Recuperados. saldos, configurações, jogos e bibliotecas de sistema foram preservados.","es":"Recuperados. se conservaron ahorros, configuraciones, juegos y bibliotecas de sistema.","fr":"Les économies, les paramètres, les jeux et les bibliothèques système ont été conservés.","de":"Sparen, Einstellungen, Spiele und Systembibliotheken wurden bewahrt.","it":"Risparmio, impostazioni, giochi e biblioteche di sistema sono state conservate.","ja":"保存、設定、ゲーム、システムライブラリが保存されました。","ko":"저장, 설정, 게임 및 시스템 도서관이 보존되었습니다.","zh-CN":"保存,保存,游戏和系统图书馆保存。","ar":"تم استردادها. تم الحفاظ على الادخار والإعدادات والألعاب ومكتبات النظام.","fa":"ذخایر، تنظیمات، بازی ها و کتابخانه های سیستم حفظ شده است.","ru":"Сохранились сбережения, настройки, игры и системные библиотеки.","uk":"Збереглися: збереглися спаси, налаштування, ігри та системні бібліотеки.","pt-PT":"Recuperados. saldos, configurações, jogos e bibliotecas de sistema foram preservados.","zh-TW":"保存,保存,遊戲和系統圖書館保存。"},"A complete SHA-256 is required":{"en":"A complete SHA-256 is required","pt-BR":"Um total de SHA-256 é necessário","es":"Se requiere un total de SHA-256","fr":"Un total de SHA-256 est requis","de":"Eine vollständige SHA-256 ist erforderlich","it":"È necessario un totale SHA-256","ja":"完全SHA-256 が必要です。","ko":"전체 SHA-256이 필요합니다.","zh-CN":"需要完整的SHA-256","ar":"مطلوب SHA-256 كاملة","fa":"SHA-256 مورد نیاز است","ru":"Необходимо полный SHA-256","uk":"Повний SHA-256 потрібен","pt-PT":"Um total de SHA-256 é necessário","zh-TW":"需要完整的SHA-256"},"Package SHA-256 mismatch":{"en":"Package SHA-256 mismatch","pt-BR":"Pacote SHA-256 mismatch","es":"paquete SHA-256 mismatch","fr":"Package SHA-256 mismatch","de":"Packung SHA-256 Missmatch","it":"Pacchetto SHA-256 mismatch","ja":"パッケージ SHA-256 ミスマッチ","ko":"패키지 SHA-256 오류","zh-CN":"包装 SHA-256 错误","ar":"حزمة SHA-256 غير متوافقة","fa":"بسته بندی SHA-256 اشتباه","ru":"Пакет SHA-256 несовместимый","uk":"Пакет SHA-256 несумісні","pt-PT":"Pacote SHA-256 mismatch","zh-TW":"包裝 SHA-256 錯誤"},"Package exceeds size limit":{"en":"Package exceeds size limit","pt-BR":"Pacote excede o limite de tamanho","es":"El paquete supera el límite de tamaño","fr":"Le paquet dépasse la limite de taille","de":"Packung überschreitet Größengrenze","it":"Il pacchetto supera il limite di dimensione","ja":"パッケージサイズ制限を超える","ko":"패키지 크기 제한을 초과","zh-CN":"包装超越尺寸限制","ar":"الحزمة تتجاوز الحد الأقصى","fa":"بسته بندی حداکثر اندازه","ru":"Пакет превышает размер","uk":"Пакет перевищує обмеження розмірів","pt-PT":"Pacote excede o limite de tamanho","zh-TW":"包裝超越尺寸限制"},"Too many ZIP entries":{"en":"Too many ZIP entries","pt-BR":"Muito ZIP entradas","es":"Tanto de entradas ZIP","fr":"Trop de ZIP entrées","de":"Zu viele ZIP-Einträge","it":"Tanti gli ingressi ZIP","ja":"沢山のZIP入力","ko":"너무 많은 ZIP 입력","zh-CN":"太多的ZIP输入","ar":"الكثير من إدخالات ZIP","fa":"تعداد زیادی از ZIP ها","ru":"Слишком много входов ZIP","uk":"Занадто багато ZIP входів","pt-PT":"Muito ZIP entradas","zh-TW":"太多的ZIP輸入"},"Invalid ZIP path":{"en":"Invalid ZIP path","pt-BR":"Caminho ZIP Invalido","es":"Puerta ZIP Invalida","fr":"Route ZIP Invalide","de":"Invalid ZIP-Path","it":"Invalidità di ZIP","ja":"障害 ZIP コース","ko":"장애 ZIP 경로","zh-CN":"缺陷的ZIP路线","ar":"مسار ZIP المعاكس","fa":"مسیر ZIP معلول","ru":"Невыполненный ZIP путь","uk":"Заборонений ZIP шлях","pt-PT":"Caminho ZIP Invalido","zh-TW":"缺陷的ZIP路線"},"ZIP must contain one ZeraPS5 package folder":{"en":"ZIP must contain one ZeraPS5 package folder","pt-BR":"O ZIP deve conter uma pasta de pacote ZeraPS5","es":"ZIP debe contener una fila de paquetes ZeraPS5","fr":"ZIP doit contenir une boîte de ZeraPS5","de":"ZIP muss eine ZeraPS5 Package-Folde enthalten","it":"ZIP deve contenere una cartella di pacchetto ZeraPS5","ja":"ZIP は 1 つの ZeraPS5 パッケージ フォルダーを含む必要があります。","ko":"ZIP에는 ZeraPS5 패키지 폴더가 포함되어야 합니다.","zh-CN":"ZIP 必须包含一个 ZeraPS5 包文件夹","ar":"يجب أن يحتوي ZIP على مجلد واحد ZeraPS5","fa":"ZIP باید شامل یک پوشه بسته ZeraPS5 باشد.","ru":"ZIP должен содержать одну папку ZeraPS5","uk":"ZIP повинен містити один папку ZeraPS5","pt-PT":"O ZIP deve conter uma pasta de pacote ZeraPS5","zh-TW":"ZIP 必須包含一個 ZeraPS5 包文件夾"},"Duplicate ZIP entry":{"en":"Duplicate ZIP entry","pt-BR":"Duplicado ZIP entrada","es":"Duplicado de entrada ZIP","fr":"Entrée ZIP","de":"Duplikate ZIP Eintritt","it":"Doppia ingresso ZIP","ja":"ダブル ZIP 入力","ko":"ZIP 입장기","zh-CN":"双重ZIP入口","ar":"إدخال ZIP","fa":"ورود ZIP","ru":"Двойный ZIP вход","uk":"Двоповерховий ZIP вхід","pt-PT":"Duplicado ZIP entrada","zh-TW":"雙重ZIP入口"},"ZIP symbolic link rejected":{"en":"ZIP symbolic link rejected","pt-BR":"Link simbólico de ZIP rejeitado","es":"El enlace simbólico de ZIP se rechaza","fr":"Le lien symbolique ZIP rejeté","de":"ZIP Symbol Link abgelehnt","it":"Il link simbolico ZIP è stato respinto","ja":"ZIP シンボルリンク拒否","ko":"ZIP 상징적 링크 거부","zh-CN":"ZIP 符号链接被拒绝","ar":"رفضت الارتباط الرمزية ZIP","fa":"لینک های نمادین ZIP رد شد","ru":"ЗИП символический линк отвержен","uk":"Зіп символічний зв'язок відхилено","pt-PT":"Link simbólico de ZIP rejeitado","zh-TW":"ZIP 符號鏈接被拒絕"},"Unexpected package directory":{"en":"Unexpected package directory","pt-BR":"Inesperado Pacote de Direito","es":"Directorios de paquetes inesperados","fr":"Directory de paquets inattendus","de":"Unerwartete Paket-Dokumentation","it":"Il nuovo package directory","ja":"予想外のパッケージディレクトリ","ko":"예기치 않은 패키지 디렉토리","zh-CN":"未知包装目录","ar":"دليل الحزمة غير المتوقعة","fa":"ناگهانی بسته بندی","ru":"Неожиданный пакетный каталог","uk":"Неочікуваний пакетний каталог","pt-PT":"Inesperado Pacote de Direito","zh-TW":"未知包裝目錄"},"Unexpected package file":{"en":"Unexpected package file","pt-BR":"arquivo de pacote inesperado","es":"Archivo de paquete inesperado","fr":"Fichiers de paquets inattendus","de":"Unerwartete Paketdateien","it":"Il file di pacchetto inaspettato","ja":"予想外のパッケージファイル","ko":"예기치 않은 파일 패키지","zh-CN":"未知的包装文件","ar":"ملف الحزمة غير المتوقعة","fa":"فایل های ناگهانی","ru":"Неожиданные пакеты файлов","uk":"Неочікувані пакети","pt-PT":"ficheiro de pacote inesperado","zh-TW":"未知的包裝文件"},"Expanded package exceeds size limit":{"en":"Expanded package exceeds size limit","pt-BR":"Pacote expandido excede o limite de tamanho","es":"El paquete ampliado supera el límite de tamaño","fr":"Le paquet élargi dépasse la limite de taille","de":"Vergrößerte Packung übersteigt Größengrenze","it":"Il pacchetto esteso supera il limite di dimensioni","ja":"拡張パッケージサイズ制限を超える","ko":"확장된 패키지는 크기 제한을 초과합니다.","zh-CN":"扩展包超越尺寸限制","ar":"الحزمة الموسعة تتجاوز الحد الأقصى للحجم","fa":"بسته بندی بزرگتر از حد اندازه","ru":"Расширенный пакет превышает размерный лимит","uk":"Розширений пакет перевищує обмеження розмірів","pt-PT":"Pacote expandido excede o limite de tamanho","zh-TW":"擴展包超越尺寸限制"},"Invalid payload manifest":{"en":"Invalid payload manifest","pt-BR":"Invalidação de pagamento manifesto","es":"Invalidación de la carga manifiesta","fr":"Le défaut de la charge manifeste","de":"Invalid Payload manifestiert","it":"Il manifesto di incapacità","ja":"無効の支払いマニスト","ko":"불법 지불 명백","zh-CN":"无效的付款表明","ar":"إعلانات الخصم المعاكس","fa":"معافیت پرداخت مانیفست","ru":"Невыполненный платежный знак","uk":"Недійсний випадок оплати","pt-PT":"Invalidação de pagamento manifesto","zh-TW":"無效的付款表明"},"Wrong update version, channel or architecture":{"en":"Wrong update version, channel or architecture","pt-BR":"Versão, canal ou arquitetura incorretamente atualizada","es":"Versión, canal o arquitectura incorrecta","fr":"Une version, un canal ou une architecture de mise à jour incorrecte","de":"Fehl aktualisierte Version, Kanal oder Architektur","it":"Versione, canale o architettura aggiornata","ja":"バージョン、チャンネル、アーキテクチャの更新が間違っている","ko":"잘못된 업데이트 버전, 채널 또는 아키텍처","zh-CN":"错误更新版本、频道或架构","ar":"تحديث نسخة أو قناة أو الهندسة المعمارية الخاطئة","fa":"نسخه، کانال یا معماری به روز رسانی اشتباه","ru":"Неправильная версия, канал или архитектура","uk":"Помилковий варіант, канал або архітектура","pt-PT":"Versão, canal ou arquitetura incorretamente atualizada","zh-TW":"錯誤更新版本、頻道或架構"},"Invalid manifest file":{"en":"Invalid manifest file","pt-BR":"Invalidação do arquivo manifesto","es":"Invalida el archivo manifest","fr":"Fiche manifeste invalide","de":"Invalid Manifest Datei","it":"Fatto manifesto invalido","ja":"無効のマニストファイル","ko":"무효 파일","zh-CN":"未经授权的文件","ar":"ملف ملاحظة غير صالح","fa":"فایلهای مانیفست نامناسب","ru":"Невалидный манифест","uk":"Недійсний файл","pt-PT":"Invalidação do ficheiro manifesto","zh-TW":"未經授權的文件"},"A directory conflicts with a program file":{"en":"A directory conflicts with a program file","pt-BR":"Conflictos de diretório com um arquivo de programa","es":"Conflictos de directorio con un archivo de programa","fr":"Un dossier de conflit avec un fichier de programme","de":"Ein Katalog Konflikte mit einem Programmdatei","it":"Un file di directory conflitto con un file di programma","ja":"プログラムファイルとのディレクトリの紛争","ko":"디렉토리 파일과 프로그램 파일의 갈등","zh-CN":"与程序文件发生冲突的目录","ar":"تصادمات الدليل مع ملف البرنامج","fa":"یک دایرکتوری درگیری با یک فایل برنامه","ru":"Конфликт каталога с программой файла","uk":"Конфлікт каталогу з програмою файлу","pt-PT":"Conflictos de diretório com um ficheiro de programa","zh-TW":"與程序文件發生衝突的目錄"},"Payload file checksum mismatch":{"en":"Payload file checksum mismatch","pt-BR":"Arquivo da tag Checksum Mismatch","es":"Archivo de la etiqueta Checksum Mismatch","fr":"Fiche de fichier Checksum Mismatch","de":"Datei für Checksum Mismatch","it":"Il file di pagamento checksum mismatch","ja":"ファイルチェックイン ミスマッチ","ko":"파일 체크스미 미스미치","zh-CN":"查询 查询 错误 错误","ar":"تحميل ملف Checksum Mismatch","fa":"دانلود فایل چکسم اشتباه","ru":"Загрузка файла Checksum Mismatch","uk":"Завантажити файл Checksum Mismatch","pt-PT":"Arquivo da tag Checksum Mismatch","zh-TW":"查詢 查詢 錯誤 錯誤"},"Incomplete update payload":{"en":"Incomplete update payload","pt-BR":"Atualização de Payload","es":"Desactualizado el pago","fr":"Actualités de payload incomplète","de":"Unvollständige Aktualisierung Payload","it":"aggiornamento di payload","ja":"無効更新 パイロット","ko":"업데이트 불완전한 지불","zh-CN":"未完成的更新 付款","ar":"تحديث غير كامل","fa":"به روز رسانی بی نظیر","ru":"Неполный обновленный платежный заряд","uk":"Недостатня оновлення платіжних платежів","pt-PT":"Atualização de Payload","zh-TW":"未完成的更新 付款"},"Installed file checksum mismatch":{"en":"Installed file checksum mismatch","pt-BR":"Ficha de verificação de arquivos de checksum mismatch","es":"Archivo de la etiqueta Checksum Mismatch","fr":"Fichiers de contrôle mismatch","de":"Installation von Dateien Checksum Mismatch","it":"Il file checksum mismatch","ja":"ファイルチェックスムミスマッチ","ko":"설치된 파일 checksum mismatch","zh-CN":"安装文件 Checksum Mismatch","ar":"الملفات المكتوبة Checksum Mismatch","fa":"دانلود فایل چکسم اشتباه","ru":"Загруженный файл Checksum Mismatch","uk":"Завантажити файл Checksum Mismatch","pt-PT":"Ficha de verificação de ficheiros de checksum mismatch","zh-TW":"安裝文件 Checksum Mismatch"},"Injected copy failure":{"en":"Injected copy failure","pt-BR":"Injeção de cópia falha","es":"Fracaso de copia inyectada","fr":"Faillite de copie injectée","de":"Injektionsverlust Kopie","it":"Iniezione di copia fallita","ja":"注射コピー失敗","ko":"복사 실패 복사 실패","zh-CN":"注入复制失败","ar":"فشل نسخة مدرجة","fa":"کپی ورودی ناپدید شده","ru":"Инъекция копирования неудачи","uk":"Введення копії з невдачею","pt-PT":"Injeção de cópia falha","zh-TW":"注入複製失敗"},"Update installed. Previous program files retained in user\\updates\\transaction\\backup.":{"en":"Update installed. Previous program files retained in user\\updates\\transaction\\backup.","pt-BR":"Atualizar instalado. arquivos de programa anteriores mantidos em user\\updates\\transaction\\backup.","es":"Actualizar instalado. archivos de programa anteriores conservados en user\\updates\\transaction\\backup.","fr":"Actualisation installée. précédent fichier de programme conservé dans user\\updates\\transaction\\backup.","de":"Update installiert. Vorherige Programmdateien in user\\updates\\transaction\\backup. gespeichert","it":"Aggiornamento installato. file di programma precedenti conservati in user\\updates\\transaction\\backup.","ja":"更新がインストールされました. 以前のプログラムファイルはuser\\updates\\transaction\\backup.に保存されました","ko":"업데이트 설치. 이전 프로그램 파일은 user\\updates\\transaction\\backup.에 저장","zh-CN":"更新安装. 以前的程序文件保留在 user\\updates\\transaction\\backup.","ar":"تحديث مثبت.الملفات السابقة البرنامج تم الاحتفاظ بها في user\\updates\\transaction\\backup.","fa":"به روز رسانی نصب شده. فایل های برنامه قبلی در user\\updates\\transaction\\backup. ذخیره شده است","ru":"Актуализация установлена. предыдущие программные файлы сохраняются в user\\updates\\transaction\\backup.","uk":"Попередні програми файли зберігаються в user\\updates\\transaction\\backup.","pt-PT":"Atualizar instalado. ficheiros de programa anteriores mantidos em user\\updates\\transaction\\backup.","zh-TW":"更新安裝. 以前的程序文件保留在 user\\updates\\transaction\\backup."},"Update failed. Program files were restored when possible. Run user\\updates\\Recuperar-ZeraPS5.ps1 with -InstallRoot and -Recover if interrupted.":{"en":"Update failed. Program files were restored when possible. Run user\\updates\\Recuperar-ZeraPS5.ps1 with -InstallRoot and -Recover if interrupted.","pt-BR":"Atualização falhou. arquivos de programa foram restaurados quando possível. executar user\\updates\\Recuperar-ZeraPS5.ps1 com -InstallRoot e -Recover se interrompido.","es":"Actualizar no. Los archivos de programa se restauraron cuando sea posible. ejecutar user\\updates\\Recuperar-ZeraPS5.ps1 con -InstallRoot y -Recover si interrumpido.","fr":"Mise à jour échoue. les fichiers de programme ont été restaurés lorsque possible. Exécuter user\\updates\\Recuperar-ZeraPS5.ps1 avec -InstallRoot et -Recover si interrompu.","de":"Update fehlte. Programmdateien wurden wiederhergestellt, wenn möglich. user\\updates\\Recuperar-ZeraPS5.ps1 mit -InstallRoot und -Recover ausgeführt, wenn es unterbrochen ist.","it":"Il programma è stato ripristinato quando possibile. eseguire user\\updates\\Recuperar-ZeraPS5.ps1 con -InstallRoot e -Recover se interrotto.","ja":"更新が失敗しました. 可能な限りプログラムファイルが復元されました. 停止した場合 -InstallRoot と -Recover で user\\updates\\Recuperar-ZeraPS5.ps1 を実行します。","ko":"업데이트 실패. 프로그램 파일이 가능한 경우 복구되었습니다. -InstallRoot 및 -Recover를 사용하여 user\\updates\\Recuperar-ZeraPS5.ps1을 실행하면 중단됩니다.","zh-CN":"更新失败了. 程序文件在可能时恢复了. 运行 user\\updates\\Recuperar-ZeraPS5.ps1 使用 -InstallRoot 和 -Recover 如果中断。","ar":"تم استعادة ملفات البرنامج عند الإمكان. تشغيل user\\updates\\Recuperar-ZeraPS5.ps1 مع -InstallRoot و -Recover إذا تم إيقافها.","fa":"به روز رسانی شکست خورده است. فایل های برنامه در صورت امکان بازگردانده شده است. اجرا user\\updates\\Recuperar-ZeraPS5.ps1 با -InstallRoot و -Recover در صورت قطع.","ru":"Программные файлы были восстановлены, когда это возможно. запустить user\\updates\\Recuperar-ZeraPS5.ps1 с -InstallRoot и -Recover, если они были прерваны.","uk":"Програма файлів відновлюється, коли це можливо. user\\updates\\Recuperar-ZeraPS5.ps1 -InstallRoot і -Recover, якщо вони перервані.","pt-PT":"Atualização falhou. ficheiros de programa foram restaurados quando possível. executar user\\updates\\Recuperar-ZeraPS5.ps1 com -InstallRoot e -Recover se interrompido.","zh-TW":"更新失敗了. 程序文件在可能時恢復了. 運行 user\\updates\\Recuperar-ZeraPS5.ps1 使用 -InstallRoot 和 -Recover 如果中斷。"}}
'@ | ConvertFrom-Json
# END GENERATED UPDATE TRANSLATIONS
# Read only the interface preference; never alter console or per-game settings.
if (-not $Language) {
    $ini = Join-Path $InstallRoot 'user/config/zeraps5.ini'
    if (Test-Path -LiteralPath $ini -PathType Leaf) {
        $section = ''
        foreach ($line in Get-Content -LiteralPath $ini -Encoding UTF8) {
            if ($line -match '^\s*\[([^]]+)\]') { $section=$Matches[1] }
            elseif ($section -eq 'interface' -and $line -match '^\s*idioma\s*=\s*([^;# ]+)') { $Language=$Matches[1] }
        }
    }
}
if ($Language -notin @('pt-BR','pt-PT','en','es','fr','de','it','ja','ko','zh-CN','zh-TW','ar','fa','ru','uk')) { $Language='pt-BR' }
function Update-Text([string]$Key) {
    $property=$script:UpdateMessages.PSObject.Properties[$Key]
    if ($null -ne $property) { return $property.Value.$Language }
    # Unknown OS errors are represented by a stable code in the result file.
    return $Key
}
$root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $root 'ZeraPS5.exe') -PathType Leaf)) { throw (Update-Text 'InstallRoot must contain ZeraPS5.exe') }
$work = Join-Path $root 'user\updates\transaction'
$journal = Join-Path $work 'journal.json'
$log = Join-Path $root 'user\updates\result.txt'
$script:FileFailure = $null
$lockLog = Join-Path $root 'user\updates\file-locks.jsonl'
$installerLock = $null

# Windows scanners can retain a file briefly after the launcher has exited.
# Retry only sharing/byte-range locks, never permissions or corrupt payloads.
# Diagnostics contain package-relative names, never host/account paths.
function Invoke-FileOperation([string]$Operation, [string]$RelativePath, [scriptblock]$Action) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $attempts = 0
    while ($true) {
        $attempts++
        try {
            $value = & $Action
        } catch {
            $failure = $_
            $exception = $_.Exception
            while ($exception.InnerException) { $exception = $exception.InnerException }
            $code = $exception.HResult -band 0xffff
            $details = [ordered]@{operation=$Operation;file=$RelativePath;win32=$code;attempts=$attempts;waited_ms=$timer.ElapsedMilliseconds;resolved=$false}
            if ($code -notin @(32,33) -or $timer.Elapsed.TotalSeconds -ge $FileLockTimeoutSeconds) {
                $script:FileFailure = $details
                if ($code -in @(32,33)) { Write-LockEvent $details }
                throw $failure
            }
            Start-Sleep -Milliseconds 200
            continue
        }
        if ($attempts -gt 1) {
            $details.resolved = $true
            $details.attempts = $attempts
            $details.waited_ms = $timer.ElapsedMilliseconds
            Write-LockEvent $details
        }
        return $value
    }
}
function Write-LockEvent($Details) {
    # A diagnostic failure must not turn a successful install into a rollback.
    try { $Details | ConvertTo-Json -Compress | Add-Content -LiteralPath $lockLog -Encoding UTF8 } catch {}
}

function Assert-LocalPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($root+'\', [StringComparison]::OrdinalIgnoreCase)) { throw (Update-Text 'Path escaped installation') }
    $cur = $full
    while ($cur -and $cur.Length -ge $root.Length) {
        if (Test-Path -LiteralPath $cur) {
            if ((Get-Item -LiteralPath $cur -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw (Update-Text 'Reparse points are not update destinations') }
        }
        $cur = [IO.Path]::GetDirectoryName($cur)
    }
}
Assert-LocalPath $work
New-Item -ItemType Directory -Force (Split-Path $work) | Out-Null

function File-Hash([string]$Path) {
    Invoke-FileOperation 'hash' ([IO.Path]::GetFileName($Path)) {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        $stream = $null
        try {
            $stream = [IO.File]::OpenRead($Path)
            return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')
        } finally {
            if ($stream) { $stream.Dispose() }
            $algorithm.Dispose()
        }
    }
}

function Allowed([string]$Path) {
    if ($Path -match '[\\:\x00-\x1f]' -or $Path.StartsWith('/') -or $Path.Length -gt 180) { return $false }
    foreach ($part in $Path.Split('/')) {
        if (-not $part -or $part -eq '.' -or $part -eq '..' -or $part -match '[. ]$|[<>"|?*]' -or $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') { return $false }
    }
    return $Path -match '^(?i:ps5c\.exe|ZeraPS5\.exe|LICENSE|CHANGELOG\.md|CREDITS\.txt|LEIA-ME\.txt|update-files\.json|tools/apply-update\.ps1|data/(?:nid_names|entitlements)\.txt|(?:licenses|docs|sources)/[^/]+)$'
}
function Save-Journal($State) {
    $tmp = $journal+'.tmp'
    $json = $State | ConvertTo-Json -Depth 6
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    Invoke-FileOperation 'journal-temp' 'user/updates/transaction/journal.json.tmp' {
        $fs = [IO.File]::Open($tmp,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $fs.Write($bytes,0,$bytes.Length); $fs.Flush($true) } finally { $fs.Dispose() }
    }
    Invoke-FileOperation 'journal' 'user/updates/transaction/journal.json' {
        Move-Item -LiteralPath $tmp -Destination $journal -Force
    }
}
function Rollback($State) {
    if ($State.phase -ne 'applying') { return }
    foreach ($f in $State.files) {
        if (-not (Allowed $f.path)) { throw (Update-Text 'Invalid recovery journal') }
        $dst = Join-Path $root $f.path
        Assert-LocalPath $dst
        if ($f.existed) {
            $bak = Join-Path (Join-Path $work 'backup') $f.path
            Assert-LocalPath $bak
            if ((File-Hash $bak) -ne $f.old_sha256) { throw (Update-Text 'Recovery backup is damaged') }
            # Do not overwrite untouched files (including an unchanged file
            # that a scanner or another launcher still has open).
            if ((Test-Path -LiteralPath $dst -PathType Leaf) -and (File-Hash $dst) -eq $f.old_sha256) { continue }
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            Invoke-FileOperation 'restore' $f.path { Copy-Item -LiteralPath $bak -Destination $dst -Force }
            if ((File-Hash $dst) -ne $f.old_sha256) { throw (Update-Text 'Recovery backup is damaged') }
        } elseif (Test-Path -LiteralPath $dst) {
            Invoke-FileOperation 'remove' $f.path { Remove-Item -LiteralPath $dst -Force }
        }
    }
    $State.phase = 'rolledback'; Save-Journal $State
}
function Wait-ForLauncher {
    if ($WaitPid) {
        $p = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
        if ($p) {
            if (-not [string]::Equals($p.Path,(Join-Path $root 'ZeraPS5.exe'),[StringComparison]::OrdinalIgnoreCase)) { throw (Update-Text 'WaitPid is not this installation launcher') }
            if (-not $p.WaitForExit(120000)) { throw (Update-Text 'Launcher did not close; no files were replaced') }
        }
    }
    $running = Get-Process -Name ps5c,ZeraPS5 -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and [string]::Equals((Split-Path $_.Path),$root,[StringComparison]::OrdinalIgnoreCase) } catch { $false }
    }
    if ($running) { throw (Update-Text 'Close games and launcher in this installation before updating') }
}

try {
    # Waiting on a scanner must not let a second updater replace the first
    # transaction's stage, journal or backups. This handle is outside $work
    # and Windows releases it even if this helper is interrupted.
    $lockPath = Join-Path $root 'user/updates/installer.lock'
    Assert-LocalPath $lockPath
    $installerLock = Invoke-FileOperation 'installer-lock' 'user/updates/installer.lock' {
        [IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    }
    Wait-ForLauncher
    if (Test-Path -LiteralPath $journal) {
        $old = Get-Content -LiteralPath $journal -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($old.phase -eq 'applying') { Rollback $old }
    }
    if ($Recover) {
        (Update-Text 'Recovered. Saves, settings, games and system libraries were preserved.') | Set-Content -LiteralPath $log -Encoding UTF8
        if ($Restart) { Start-Process -FilePath (Join-Path $root 'ZeraPS5.exe') -WorkingDirectory $root -WindowStyle Normal }
        exit 0
    }
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw (Update-Text 'A complete SHA-256 is required') }
    if ((File-Hash $Package) -ne $ExpectedSha256) { throw (Update-Text 'Package SHA-256 mismatch') }
    if ((Get-Item -LiteralPath $Package).Length -gt 512MB) { throw (Update-Text 'Package exceeds size limit') }
    if (Test-Path -LiteralPath $work) {
        Assert-LocalPath $work
        Remove-Item -LiteralPath $work -Recurse -Force
    }
    New-Item -ItemType Directory -Force $work | Out-Null
    $stage = Join-Path $work 'stage'; $backup = Join-Path $work 'backup'
    New-Item -ItemType Directory -Path $stage,$backup | Out-Null
    $archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Package))
    try {
        $names = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $top = $null; [long]$total = 0
        if ($archive.Entries.Count -gt 4096) { throw (Update-Text 'Too many ZIP entries') }
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            if ($name.Contains('\') -or $name.StartsWith('/') -or $name -match ':|\x00') { throw (Update-Text 'Invalid ZIP path') }
            $parts = $name.Split('/')
            if (-not $top) { $top = $parts[0] }
            if ($top -notmatch '^ZeraPS5-[A-Za-z0-9._-]+$' -or $parts[0] -cne $top) { throw (Update-Text 'ZIP must contain one ZeraPS5 package folder') }
            if (-not $names.Add($name.TrimEnd('/'))) { throw (Update-Text 'Duplicate ZIP entry') }
            if (($entry.ExternalAttributes -shr 16 -band 0xf000) -eq 0xa000) { throw (Update-Text 'ZIP symbolic link rejected') }
            $rel = $name.Substring($top.Length).TrimStart('/')
            if ($name.EndsWith('/')) {
                if ($rel -and $rel.TrimEnd('/') -notmatch '^(data|docs|licenses|sources|tools|games|system|system/modules)$') { throw (Update-Text 'Unexpected package directory') }
                continue
            }
            if (-not (Allowed $rel) -and $rel -notin @('games/LEIA-ME.txt','system/LEIA-ME.txt')) { throw (Update-Text 'Unexpected package file') }
            $total += $entry.Length
            if ($total -gt 512MB) { throw (Update-Text 'Expanded package exceeds size limit') }
        }
    } finally { $archive.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory([IO.Path]::GetFullPath($Package),$stage)
    $payload = Join-Path $stage $top
    $manifest = Get-Content -LiteralPath (Join-Path $payload 'update-files.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.build -lt 1 -or $manifest.files.Count -gt 2048) { throw (Update-Text 'Invalid payload manifest') }
    if ($manifest.channel -ne 'stable' -or $manifest.platform -ne 'windows-x86_64' -or ($ExpectedBuild -and $manifest.build -ne $ExpectedBuild)) { throw (Update-Text 'Wrong update version, channel or architecture') }
    $paths = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $plan = @()
    foreach ($f in $manifest.files) {
        if (-not (Allowed $f.path) -or $f.path -eq 'update-files.json' -or -not $paths.Add($f.path) -or $f.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw (Update-Text 'Invalid manifest file') }
        $src = Join-Path $payload $f.path; $dst = Join-Path $root $f.path
        Assert-LocalPath $dst
        if (Test-Path -LiteralPath $dst -PathType Container) { throw (Update-Text 'A directory conflicts with a program file') }
        if ((Get-Item -LiteralPath $src).Length -ne $f.size -or (File-Hash $src) -ne $f.sha256) { throw (Update-Text 'Payload file checksum mismatch') }
        $existed = Test-Path -LiteralPath $dst -PathType Leaf
        $sha = ''
        if ($existed) {
            $bak = Join-Path $backup $f.path
            New-Item -ItemType Directory -Force (Split-Path $bak) | Out-Null
            Invoke-FileOperation 'backup' $f.path { Copy-Item -LiteralPath $dst -Destination $bak }
            $sha = File-Hash $bak
        }
        $plan += [ordered]@{path=$f.path;existed=$existed;old_sha256=$sha;new_sha256=$f.sha256}
    }
    foreach ($required in @('ps5c.exe','ZeraPS5.exe','tools/apply-update.ps1')) { if (-not $paths.Contains($required)) { throw (Update-Text 'Incomplete update payload') } }
    # Check writable destinations before the first replacement. Opening with
    # FileMode.Open does not truncate them; an executable image still in use
    # or a reader denying writes is retried without touching program bytes.
    foreach ($f in $plan) {
        if ($f.existed) {
            $dst = Join-Path $root $f.path
            # Copy-Item -Force already handles the readonly attribute; a write
            # probe would reject it as access denied before that can happen.
            if ((Get-Item -LiteralPath $dst -Force).IsReadOnly) { continue }
            Invoke-FileOperation 'preflight' $f.path {
                $probe = [IO.File]::Open($dst,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
                $probe.Dispose()
            }
        }
    }
    $state = [ordered]@{schema=1;phase='applying';build=$manifest.build;files=@($plan)}
    Save-Journal $state
    $count = 0
    try {
        foreach ($f in $plan) {
            $src = Join-Path $payload $f.path; $dst = Join-Path $root $f.path
            Assert-LocalPath $dst
            New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
            Invoke-FileOperation 'install' $f.path { Copy-Item -LiteralPath $src -Destination $dst -Force }
            if ((File-Hash $dst) -ne $f.new_sha256) { throw (Update-Text 'Installed file checksum mismatch') }
            $count++
            if ($env:PS5C_UPDATE_TEST_HOOKS -eq '1') {
                if ($TestFailAfter -and $count -eq $TestFailAfter) { throw (Update-Text 'Injected copy failure') }
                if ($TestInterruptAfter -and $count -eq $TestInterruptAfter) { exit 37 }
            }
        }
        $state.phase = 'committed'; Save-Journal $state
    } catch { Rollback $state; throw }
    (Update-Text 'Update installed. Previous program files retained in user\updates\transaction\backup.') | Set-Content -LiteralPath $log -Encoding UTF8
    if ($Restart) { Start-Process -FilePath (Join-Path $root 'ZeraPS5.exe') -WorkingDirectory $root -WindowStyle Normal }
    exit 0
} catch {
    $failure = $_
    # Paths and account names are deliberately absent from the shareable result.
    $result=Update-Text 'Update failed. Program files were restored when possible. Run user\updates\Recuperar-ZeraPS5.ps1 with -InstallRoot and -Recover if interrupted.'
    $reason=$_.Exception.Message
    $known=$false
    foreach ($entry in $script:UpdateMessages.PSObject.Properties) {
        if ($entry.Value.$Language -ceq $reason) { $known=$true; break }
    }
    if ($known) { $result+="`n"+$reason }
    else { $result+="`nHRESULT: 0x"+$_.Exception.HResult.ToString('X8') }
    $result | Set-Content -LiteralPath $log -Encoding UTF8
    if ($script:FileFailure) {
        try { $script:FileFailure | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'user/updates/file-error.json') -Encoding UTF8 } catch {}
    }
    Write-Error $failure
    exit 1
} finally {
    if ($installerLock) { $installerLock.Dispose() }
}
