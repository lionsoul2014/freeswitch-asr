-- answer the call
session:answer();

-- flite tts
-- session:set_tts_params("flite", "slt");
-- session:speak("Hello there, i am the artificial intelligence telephone system");

-- yytts
session:set_tts_params("yytts", "zhilingf");
session:speak("你好，我是人工智能电话系统");

session:sleep(1000);

-- hangup
session:hangup();