# Kilogram Desktop

Клон [официального десктоп-клиента Телеграм][telegram_desktop] с набором дополнительных возможностей.

Этот репозиторий содержит перечень измененных и добавленных файлов [официального репозитория десктоп-клиента Телеграм][telegram_desktop] для реализации функциональности Kilogram. Для компиляции и сборки скачайте официальную версию и замените в ней соответствующие файлы файлами из данного репозитория. Если код официальной версии претерпит существенные изменения, возможно потребуется ручная копипаста нужных блоков кода с последующей доработкой напильником :)

## Дополнительные возможности Kilogram

Суть доработок: появился т.н. режим работы "KG mode". Кнопка переключения между ним и обычным исходным "TG mode" находится в левом верхнем углу, в самом верху левого виджета меню. В KG mode пиктограммка корона (ну не было гири в исходниках, потом нарисую), в TG mode - логотип Телеграм (самолетик). При старте по умолчанию активируется KG mode, но в любой момент можно переключать туда - обратно.

Для пользователей из списка заблокированных:

* не подсвечивается "Никнейм печатает..." в топе окна
* не показываются их сообщения
* не показываются их реакции на сообщения
* не появляются иконки непрочитанных упоминаний
* не появляются иконки непрочитанных реакций
* не показываются сообщения в результатах поиска (глобального и локального в чате)

Общая идея в том, чтобы максимально скрыть пользователя от любых возможных проявлений и активностей заблокированных им пользователей - в идеале сделать эмуляцию их отсутствия в природе.

## ТУДУ

* заменить иконки логотипа
* сделать удобное редактирование списка контактов
* убрать истории от заблокированных пользователей (а может и все вообще)
* и т.д.

[//]: # (LINKS)
[telegram_desktop]: https://github.com/telegramdesktop/tdesktop
