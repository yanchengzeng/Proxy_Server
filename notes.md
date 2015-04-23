TODO
- error handling
- way to kill downstream handling thread

Notes
- Browser connection/request characterization
	* Browser creates many connections (>30 for Bruce Maggs page)
	* Only a very few of these connections have multiple requests
	? Do all connections have different socket descriptors?
