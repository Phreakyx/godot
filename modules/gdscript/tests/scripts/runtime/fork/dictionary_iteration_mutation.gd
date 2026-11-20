# Fork contract: what `for k in dict` visits when the loop changes the dictionary.

func make_dict() -> Dictionary:
	return { "a": 1, "b": 2, "c": 3 }

func test():
	var erase_current := make_dict()
	var visited := []
	for k in erase_current:
		visited.append(k)
		@warning_ignore("return_value_discarded")
		erase_current.erase(k)
	print("Erase current key: visited ", visited, ", left ", erase_current)

	var erase_other := make_dict()
	visited = []
	for k in erase_other:
		visited.append(k)
		if k == "a":
			@warning_ignore("return_value_discarded")
			erase_other.erase("b")
	print("Erase a not yet visited key: visited ", visited, ", left ", erase_other)

	var insert := make_dict()
	visited = []
	for k in insert:
		visited.append(k)
		if k == "a":
			insert["d"] = 4
	print("Insert a key: visited ", visited, ", left ", insert)

	var mixed := { "a": 1 }
	mixed[&"a"] = 2
	var key_is_string := typeof(mixed.keys()[0]) == TYPE_STRING
	print("String and StringName key: size ", mixed.size(), ", value ", mixed[&"a"],
			", key is String ", key_is_string)
